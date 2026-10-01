#include "SoundbankDatabase.h"
#include "AppDataPath.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>

namespace d110bank {

namespace {

// Same constants as D110Core.h's kSysexTones/kNumTones/kToneMemRecord, duplicated here rather
// than pulled in from D110Core.h - see this file's own header comment for why.
constexpr juce::uint32 kToneSysexAddress = 0x080000;
constexpr int kNumToneSlots = 64;
constexpr int kToneRecordSize = 256;

// Same three constants as D110Core.h's kSysexPatches/kNumPatches/kPatchRecord - kept for the
// dormant Patch decoder below (see SoundbankDatabase.h's own comment on why it's kept).
constexpr juce::uint32 kPatchSysexAddress = 0x060000;
constexpr int kNumPatches = 64;
constexpr int kPatchRecord = 128;

// Scans raw bytes for concatenated F0...F7 SysEx messages - same logic as
// PluginProcessor.cpp's own (file-local) extractSysexMessagesFromRawBytes(), duplicated
// rather than shared since that one is `static` to its own translation unit.
std::vector<std::vector<juce::uint8>> extractRawSysex(const juce::MemoryBlock &data) {
	std::vector<std::vector<juce::uint8>> messages;
	const auto *bytes = static_cast<const juce::uint8 *>(data.getData());
	const size_t len = data.getSize();

	size_t i = 0;
	while (i < len) {
		if (bytes[i] == 0xF0) {
			size_t j = i + 1;
			while (j < len && bytes[j] != 0xF7) ++j;
			if (j < len) {
				messages.emplace_back(bytes + i, bytes + j + 1);
				i = j + 1;
				continue;
			}
			break; // Unterminated trailing message - ignore it.
		}
		++i;
	}
	return messages;
}

// Same idea as PluginProcessor.cpp's own extractSysexMessagesFromMidiFile() - pulls just the
// SysEx meta-events out of a standard MIDI file, in file order.
std::vector<std::vector<juce::uint8>> extractMidiSysex(const juce::File &file) {
	std::vector<std::vector<juce::uint8>> messages;

	juce::FileInputStream stream(file);
	if (!stream.openedOk()) return messages;

	juce::MidiFile midiFile;
	if (!midiFile.readFrom(stream)) return messages;

	for (int t = 0; t < midiFile.getNumTracks(); ++t) {
		const auto *track = midiFile.getTrack(t);
		for (int e = 0; e < track->getNumEvents(); ++e) {
			const auto &message = track->getEventPointer(e)->message;
			if (!message.isSysEx()) continue;
			const auto *raw = message.getRawData();
			messages.emplace_back(raw, raw + message.getRawDataSize());
		}
	}
	return messages;
}

// Trims a raw name field at the first non-printable byte, shared by both decoders below.
juce::String trimmedName(const juce::uint8 *data, int nameChars) {
	juce::String name;
	for (int c = 0; c < nameChars; ++c) {
		const char ch = char(data[c]);
		if (ch < 32 || ch > 126) break;
		name << ch;
	}
	return name.trim();
}

// Decodes one raw F0...F7 SysEx message into zero or more whole 256-byte Tone Memory records -
// zero if it isn't a Roland DT1 write, isn't addressed inside the internal Tone area, or fails
// the Roland checksum; possibly more than one if the message (a real captured "dump the whole
// bank" capture) carries several consecutive records back to back. Mirrors the address packing
// D110Core::buildDt1Message() (D110Core.cpp) uses to build these messages in the first place,
// inverted to decode one back.
std::vector<DecodedTone> decodeTonesFromMessage(const juce::uint8 *msg, size_t len) {
	std::vector<DecodedTone> result;
	if (len < 11) return result; // F0,41,dev,model,12,a1,a2,a3,<>=1 data byte,checksum,F7
	if (msg[0] != 0xF0 || msg[len - 1] != 0xF7) return result;
	if (msg[1] != 0x41) return result; // Roland
	// msg[2] is the unit's own device ID (0-31 or "all") - varies per captured instrument,
	// not checked.
	if (msg[3] != 0x16) return result; // model: MT-32 family, which the D-110 answers to
	if (msg[4] != 0x12) return result; // DT1 ("Data set 1")

	const size_t dataLen = len - 10; // total - (F0,41,dev,model,12,a1,a2,a3) - (checksum,F7)
	if (dataLen < size_t(kToneRecordSize)) return result;

	const juce::uint8 a1 = msg[5] & 0x7f, a2 = msg[6] & 0x7f, a3 = msg[7] & 0x7f;
	const juce::uint32 target = (juce::uint32(a1) << 14) | (juce::uint32(a2) << 7) | a3;
	const juce::uint32 toneAreaBase = (((kToneSysexAddress >> 16) & 0x7f) << 14)
	                                 | (((kToneSysexAddress >> 8) & 0x7f) << 7)
	                                 | (kToneSysexAddress & 0x7f);
	if (target < toneAreaBase) return result;
	const juce::uint32 offset = target - toneAreaBase;
	if (offset % juce::uint32(kToneRecordSize) != 0
	    || offset >= juce::uint32(kNumToneSlots * kToneRecordSize))
		return result;

	// The checksum covers the address bytes plus every data byte in the message, computed
	// once for the whole message - not per-256-byte record, even if several are packed in.
	juce::uint32 sum = juce::uint32(a1) + a2 + a3;
	for (size_t i = 0; i < dataLen; ++i) sum += (msg[8 + i] & 0x7f);
	const juce::uint8 expectedChecksum = juce::uint8((128 - (sum & 0x7f)) & 0x7f);
	if ((msg[8 + dataLen] & 0x7f) != expectedChecksum) return result;

	// Two INDEPENDENT bounds (see the Patch decoder below's own comment - a real earlier bug
	// here was conflating these): `i` walks the MESSAGE's own data bytes (bounded by dataLen,
	// message-relative), while the tone slot INDEX it lands on (offset/256 + i) is bounded
	// separately by the Tone area's own total size (address-space-relative).
	for (size_t i = 0; (i + 1) * size_t(kToneRecordSize) <= dataLen
	                   && offset + juce::uint32(i) * juce::uint32(kToneRecordSize) + juce::uint32(kToneRecordSize)
	                          <= juce::uint32(kNumToneSlots * kToneRecordSize);
	     ++i) {
		DecodedTone t;
		std::memcpy(t.data, msg + 8 + i * size_t(kToneRecordSize), size_t(kToneRecordSize));
		t.name = trimmedName(t.data, 10);
		result.push_back(t);
	}
	return result;
}

// --- Patch-oriented decoding, ON HOLD (see SoundbankDatabase.h's own comment) --------------
std::vector<DecodedPatch> decodePatchesFromMessage(const juce::uint8 *msg, size_t len) {
	std::vector<DecodedPatch> result;
	if (len < 11) return result;
	if (msg[0] != 0xF0 || msg[len - 1] != 0xF7) return result;
	if (msg[1] != 0x41) return result;
	if (msg[3] != 0x16) return result;
	if (msg[4] != 0x12) return result;

	const size_t dataLen = len - 10;
	if (dataLen < size_t(kPatchRecord)) return result;

	const juce::uint8 a1 = msg[5] & 0x7f, a2 = msg[6] & 0x7f, a3 = msg[7] & 0x7f;
	const juce::uint32 target = (juce::uint32(a1) << 14) | (juce::uint32(a2) << 7) | a3;
	const juce::uint32 patchAreaBase = (((kPatchSysexAddress >> 16) & 0x7f) << 14)
	                                  | (((kPatchSysexAddress >> 8) & 0x7f) << 7)
	                                  | (kPatchSysexAddress & 0x7f);
	if (target < patchAreaBase) return result;
	const juce::uint32 offset = target - patchAreaBase;
	if (offset % juce::uint32(kPatchRecord) != 0
	    || offset >= juce::uint32(kNumPatches * kPatchRecord))
		return result;

	juce::uint32 sum = juce::uint32(a1) + a2 + a3;
	for (size_t i = 0; i < dataLen; ++i) sum += (msg[8 + i] & 0x7f);
	const juce::uint8 expectedChecksum = juce::uint8((128 - (sum & 0x7f)) & 0x7f);
	if ((msg[8 + dataLen] & 0x7f) != expectedChecksum) return result;

	for (size_t i = 0; (i + 1) * size_t(kPatchRecord) <= dataLen
	                   && offset + juce::uint32(i) * juce::uint32(kPatchRecord) + juce::uint32(kPatchRecord)
	                          <= juce::uint32(kNumPatches * kPatchRecord);
	     ++i) {
		DecodedPatch p;
		std::memcpy(p.data, msg + 8 + i * size_t(kPatchRecord), size_t(kPatchRecord));
		p.name = trimmedName(p.data, 10);
		result.push_back(p);
	}
	return result;
}

// Same address-packing formula as D110Core::buildDt1Message() (D110Core.cpp), duplicated here
// deliberately (see this file's own header comment on why SoundbankDatabase stays decoupled
// from D110Core) - UNLIKE buildDt1Message() itself, this has no 244-byte payload cap. That cap
// exists only for LIVE transmission into the firmware's own receive buffer
// (D110Core::kMaxSysexBytes - a real-time constraint), which doesn't apply to a plain .syx
// FILE: a real Roland bulk dump routinely carries a whole 256-byte Tone record in one message,
// exactly what decodeTonesFromMessage() above (and any real librarian/hardware reimporting
// this file) expects to find - chunking it the way live injection has to would produce a file
// nothing, including this app's own scanner, could read back as whole tones.
int buildToneDt1(int slot, const juce::uint8 *data256, juce::uint8 *out) {
	int n = 0;
	out[n++] = 0xF0;
	out[n++] = 0x41; // Roland
	out[n++] = 0x10; // device ID (factory default, Exclusive Unit# 17)
	out[n++] = 0x16; // model: MT-32 family, which the D-110 answers to
	out[n++] = 0x12; // DT1

	const juce::uint32 areaBase = (((kToneSysexAddress >> 16) & 0x7f) << 14)
	                             | (((kToneSysexAddress >> 8) & 0x7f) << 7) | (kToneSysexAddress & 0x7f);
	const juce::uint32 target = areaBase + juce::uint32(slot) * juce::uint32(kToneRecordSize);
	const juce::uint8 a1 = juce::uint8((target >> 14) & 0x7f);
	const juce::uint8 a2 = juce::uint8((target >> 7) & 0x7f);
	const juce::uint8 a3 = juce::uint8(target & 0x7f);
	out[n++] = a1;
	out[n++] = a2;
	out[n++] = a3;

	juce::uint32 sum = juce::uint32(a1) + a2 + a3;
	for (int i = 0; i < kToneRecordSize; ++i) {
		const juce::uint8 v = data256[i] & 0x7f;
		out[n++] = v;
		sum += v;
	}
	out[n++] = juce::uint8((128 - (sum & 0x7f)) & 0x7f);
	out[n++] = 0xF7;
	return n;
}

} // namespace

int exportTonesAsSysex(const std::vector<Entry> &tones, const juce::File &baseFile) {
	if (tones.empty()) return 0;

	const int numFiles = (int(tones.size()) + kNumToneSlots - 1) / kNumToneSlots;
	const auto dir = baseFile.getParentDirectory();
	const auto stem = baseFile.getFileNameWithoutExtension();
	const auto ext = baseFile.getFileExtension(); // includes the leading '.', or empty

	for (int chunk = 0; chunk < numFiles; ++chunk) {
		const auto target = numFiles == 1 ? baseFile
		                                   : dir.getChildFile(stem + "_"
		                                                       + juce::String(chunk + 1).paddedLeft('0', 2)
		                                                       + ext);

		juce::MemoryBlock out;
		for (int slot = 0; slot < kNumToneSlots; ++slot) {
			const size_t idx = size_t(chunk) * size_t(kNumToneSlots) + size_t(slot);
			if (idx >= tones.size()) break;
			juce::uint8 data[kToneRecordSize];
			if (!Database::readToneBytes(tones[idx], data)) continue; // file missing - skip, don't abort the export
			juce::uint8 msg[kToneRecordSize + 16];
			const int n = buildToneDt1(slot, data, msg);
			out.append(msg, size_t(n));
		}
		target.replaceWithData(out.getData(), out.getSize());
	}
	return numFiles;
}

std::vector<SplitPart> splitToneSysexFile(const juce::File &sourceFile) {
	const auto tones = decodeTonesFromFile(sourceFile);
	std::vector<SplitPart> written;
	if (tones.empty()) return written;

	const int numFiles = (int(tones.size()) + kNumToneSlots - 1) / kNumToneSlots;
	const auto dir = sourceFile.getParentDirectory();
	const auto stem = sourceFile.getFileNameWithoutExtension() + "_split";
	const auto ext = sourceFile.getFileExtension().isEmpty() ? ".syx" : sourceFile.getFileExtension();

	for (int chunk = 0; chunk < numFiles; ++chunk) {
		const auto target = numFiles == 1 ? dir.getChildFile(stem + ext)
		                                   : dir.getChildFile(stem + "_"
		                                                       + juce::String(chunk + 1).paddedLeft('0', 2)
		                                                       + ext);
		juce::MemoryBlock out;
		int count = 0;
		for (int slot = 0; slot < kNumToneSlots; ++slot) {
			const size_t idx = size_t(chunk) * size_t(kNumToneSlots) + size_t(slot);
			if (idx >= tones.size()) break;
			juce::uint8 msg[kToneRecordSize + 16];
			const int n = buildToneDt1(slot, tones[idx].data, msg);
			out.append(msg, size_t(n));
			++count;
		}
		target.replaceWithData(out.getData(), out.getSize());
		written.push_back({ target, count });
	}
	return written;
}

bool buildToneBankFile(const std::vector<DecodedTone> &tones, const juce::File &outFile) {
	if (tones.empty() || tones.size() > size_t(kNumToneSlots)) return false;

	juce::MemoryBlock out;
	for (int slot = 0; slot < int(tones.size()); ++slot) {
		juce::uint8 msg[kToneRecordSize + 16];
		const int n = buildToneDt1(slot, tones[size_t(slot)].data, msg);
		out.append(msg, size_t(n));
	}
	return outFile.replaceWithData(out.getData(), out.getSize());
}

juce::String letterGroupFor(const juce::String &name) {
	const auto trimmed = name.trim();
	if (trimmed.isEmpty()) return "_";
	const auto c0 = trimmed[0];
	if (juce::CharacterFunctions::isLetter(c0))
		return juce::String::charToString(juce::CharacterFunctions::toUpperCase(c0));
	if (juce::CharacterFunctions::isDigit(c0)) return "0-9";
	return "_";
}

std::vector<DecodedTone> decodeTonesFromFile(const juce::File &file) {
	std::vector<std::vector<juce::uint8>> messages;
	if (file.hasFileExtension("mid") || file.hasFileExtension("smf"))
		messages = extractMidiSysex(file);
	if (messages.empty()) {
		juce::MemoryBlock raw;
		if (file.loadFileAsData(raw)) messages = extractRawSysex(raw);
	}

	std::vector<DecodedTone> result;
	for (const auto &m : messages) {
		auto tones = decodeTonesFromMessage(m.data(), m.size());
		result.insert(result.end(), tones.begin(), tones.end());
	}
	return result;
}

std::vector<DecodedPatch> decodePatchesFromFile(const juce::File &file) {
	std::vector<std::vector<juce::uint8>> messages;
	if (file.hasFileExtension("mid") || file.hasFileExtension("smf"))
		messages = extractMidiSysex(file);
	if (messages.empty()) {
		juce::MemoryBlock raw;
		if (file.loadFileAsData(raw)) messages = extractRawSysex(raw);
	}

	std::vector<DecodedPatch> result;
	for (const auto &m : messages) {
		auto patches = decodePatchesFromMessage(m.data(), m.size());
		result.insert(result.end(), patches.begin(), patches.end());
	}
	return result;
}

juce::File Database::defaultRoot() {
	return d110appdata::resolve("soundbanks_db");
}

Database::Database(juce::File dbRoot) : root(std::move(dbRoot)) {}

void Database::load() {
	entries.clear();
	if (!indexFile().existsAsFile()) return;

	const auto parsed = juce::JSON::parse(indexFile());
	if (const auto *arr = parsed.getArray()) {
		for (const auto &v : *arr) {
			Entry e;
			e.displayName = v.getProperty("displayName", {}).toString();
			e.rawName = v.getProperty("rawName", {}).toString();
			e.file = root.getChildFile(v.getProperty("file", {}).toString());
			e.sourceFile = v.getProperty("sourceFile", {}).toString();
			entries.push_back(std::move(e));
		}
	}
}

void Database::saveIndex() const {
	juce::Array<juce::var> arr;
	for (const auto &e : entries) {
		auto *obj = new juce::DynamicObject();
		obj->setProperty("displayName", e.displayName);
		obj->setProperty("rawName", e.rawName);
		obj->setProperty("file", e.file.getRelativePathFrom(root));
		obj->setProperty("sourceFile", e.sourceFile);
		arr.add(juce::var(obj));
	}
	root.createDirectory();
	indexFile().replaceWithText(juce::JSON::toString(juce::var(arr)));
}

int Database::rescan(const juce::File &sourceFolder) {
	load(); // start from whatever's already on disk so this merges rather than replaces

	// How many entries already share a given rawName, INCLUDING ones added earlier in this
	// same pass - drives the " (1)"/" (2)"/... suffix. The first entry under a name (n==0)
	// gets no suffix at all.
	std::map<juce::String, int> countPerName;
	for (const auto &e : entries) ++countPerName[e.rawName];

	// A real personal library is usually organised into subfolders (by device, by artist, by
	// pack, ...) - RangedDirectoryIterator's own `isRecursive` flag walks all of them, with the
	// extension check done explicitly here (juce::File::hasFileExtension takes the same
	// semicolon-separated list findChildFiles' wildcard argument would, but without that
	// method's own recursive+wildcard interaction to have to trust).
	// FollowSymlinks::no: nothing about a tone library needs a symlink followed, and a
	// symlink loop under an unbounded recursive walk is a real (if unlikely) way to never
	// return / exhaust memory - not worth the risk for a feature that has no reason to need it.
	// Alan's request, 2026-08-28 (also for Android, where picking a whole folder isn't
	// reliable - see chooseSoundbankFiles()'s own comment in Main.cpp - so a single .zip is the
	// practical stand-in for "a folder"): a .zip's own *.syx/*.mid/*.smf entries are scanned
	// too, one level deep (a zip nested inside a zip is not).
	juce::Array<juce::File> files;
	for (const auto &entry : juce::RangedDirectoryIterator(sourceFolder, true, "*", juce::File::findFiles,
	                                                        juce::File::FollowSymlinks::no))
		if (entry.getFile().hasFileExtension("syx;mid;midi;smf;zip")) files.add(entry.getFile());

	int added = 0;

	// Shared by both the plain-file loop and the zip-entry loop below - dedup/name/write logic
	// is otherwise identical either way, only the provenance label (`sourceLabel`) differs.
	auto addDecodedTone = [&](const DecodedTone &t, const juce::String &sourceLabel) {
		const juce::String rawName = t.name.isEmpty() ? juce::String("(Unnamed)") : t.name;

		for (const auto &e : entries) {
			if (e.rawName != rawName) continue;
			juce::uint8 existing[kToneRecordSize];
			if (readToneBytes(e, existing) && std::memcmp(existing, t.data, kToneRecordSize) == 0)
				return; // already have this exact tone
		}

		const int n = countPerName[rawName]++;
		const juce::String displayName = n == 0 ? rawName : rawName + " (" + juce::String(n) + ")";

		const auto letterDir = root.getChildFile(letterGroupFor(rawName));
		letterDir.createDirectory();
		// Just needs to be unique, not derived from content - juce::Uuid avoids pulling in the
		// whole juce_cryptography module (unused elsewhere in this project) for a hash.
		const auto uniqueName = juce::Uuid().toString().substring(0, 12);
		const auto file = letterDir.getChildFile(uniqueName + ".d110tone");
		file.replaceWithData(t.data, size_t(kToneRecordSize));

		entries.push_back({ displayName, rawName, file, sourceLabel });
		++added;
	};

	for (const auto &f : files) {
		if (!f.hasFileExtension("zip")) {
			for (const auto &t : decodeTonesFromFile(f)) addDecodedTone(t, f.getFullPathName());
			continue;
		}

		// A zip entry isn't a real file on disk - decodeTonesFromFile() needs one (it opens a
		// FileInputStream/reads raw bytes by path), so each matching entry is extracted to a
		// throwaway temp file, decoded through the exact same path as everything else, then
		// deleted - simpler than a second, memory-based decode path for what's a one-off,
		// not-perf-critical import.
		juce::ZipFile zip(f);
		const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
			                      .getChildFile("d110_soundbank_zip_" + juce::Uuid().toString().substring(0, 8));
		tempDir.createDirectory();
		for (int i = 0; i < zip.getNumEntries(); ++i) {
			const auto *zipEntry = zip.getEntry(i);
			if (zipEntry == nullptr) continue;
			const juce::File asFile(zipEntry->filename); // just to read its own extension
			if (!asFile.hasFileExtension("syx;mid;midi;smf")) continue;

			std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
			if (in == nullptr) continue;
			const auto tempFile =
				tempDir.getChildFile(juce::Uuid().toString().substring(0, 8) + "_" + asFile.getFileName());
			{
				auto out = tempFile.createOutputStream();
				if (out == nullptr || out->writeFromInputStream(*in, -1) <= 0) {
					tempFile.deleteFile();
					continue;
				}
			}
			const auto label = f.getFileName() + " -> " + zipEntry->filename;
			for (const auto &t : decodeTonesFromFile(tempFile)) addDecodedTone(t, label);
			tempFile.deleteFile();
		}
		tempDir.deleteRecursively();
	}

	if (added > 0) saveIndex();
	return added;
}

std::vector<const Entry *> Database::byLetter(const juce::String &group) const {
	std::vector<const Entry *> result;
	for (const auto &e : entries)
		if (letterGroupFor(e.rawName) == group) result.push_back(&e);
	std::sort(result.begin(), result.end(), [](const Entry *a, const Entry *b) {
		return a->displayName.compareIgnoreCase(b->displayName) < 0;
	});
	return result;
}

int Database::countForLetter(const juce::String &group) const {
	int n = 0;
	for (const auto &e : entries)
		if (letterGroupFor(e.rawName) == group) ++n;
	return n;
}

bool Database::readToneBytes(const Entry &entry, juce::uint8 *out256) {
	juce::MemoryBlock mb;
	if (!entry.file.loadFileAsData(mb) || mb.getSize() != size_t(kToneRecordSize)) return false;
	std::memcpy(out256, mb.getData(), size_t(kToneRecordSize));
	return true;
}

bool Database::rename(const Entry &entryRef, const juce::String &newName) {
	for (auto &e : entries) {
		if (e.file != entryRef.file) continue;

		juce::uint8 data[kToneRecordSize];
		if (!readToneBytes(e, data)) return false;

		// Same space-padded/printable-ASCII (32-126) convention as
		// D110AudioProcessor::sendName() - duplicated rather than shared, see this method's own
		// header comment.
		for (int i = 0; i < 10; ++i) {
			const juce::juce_wchar ch = i < newName.length() ? newName[i] : ' ';
			data[i] = juce::uint8((ch >= 32 && ch < 127) ? ch : ' ');
		}
		e.file.replaceWithData(data, size_t(kToneRecordSize));

		const auto trimmed = trimmedName(data, 10);
		const juce::String rawName = trimmed.isEmpty() ? juce::String("(Unnamed)") : trimmed;

		// Fresh disambiguation suffix against whatever else is already under this name right
		// now - same scheme rescan()'s own addDecodedTone lambda builds incrementally during a
		// scan.
		int count = 0;
		for (const auto &other : entries)
			if (&other != &e && other.rawName == rawName) ++count;
		e.rawName = rawName;
		e.displayName = count == 0 ? rawName : rawName + " (" + juce::String(count) + ")";

		saveIndex();
		return true;
	}
	return false;
}

bool Database::remove(const Entry &entryRef) {
	for (size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].file != entryRef.file) continue;
		entries[i].file.deleteFile();
		entries.erase(entries.begin() + long(i));
		saveIndex();
		return true;
	}
	return false;
}

std::vector<std::vector<const Entry *>> Database::findDuplicateGroups() const {
	// Keyed by the 246-byte tone BODY only (offset 10..255, name excluded) so the same sound
	// captured/named differently elsewhere in the library still lands in one group. A
	// fixed-size std::array rather than std::vector as the map key - some libstdc++ versions
	// emit a bogus -Wstringop-overread warning from <=> on std::vector<unsigned char> that this
	// sidesteps entirely.
	using ToneBody = std::array<juce::uint8, size_t(kToneRecordSize - 10)>;
	std::map<ToneBody, std::vector<const Entry *>> byBody;
	for (const auto &e : entries) {
		juce::uint8 data[kToneRecordSize];
		if (!readToneBytes(e, data)) continue;
		ToneBody body;
		std::memcpy(body.data(), data + 10, body.size());
		byBody[body].push_back(&e);
	}

	std::vector<std::vector<const Entry *>> groups;
	for (auto &kv : byBody) {
		if (kv.second.size() < 2) continue;
		std::sort(kv.second.begin(), kv.second.end(), [](const Entry *a, const Entry *b) {
			return a->displayName.compareIgnoreCase(b->displayName) < 0;
		});
		groups.push_back(std::move(kv.second));
	}
	std::sort(groups.begin(), groups.end(), [](const std::vector<const Entry *> &a,
	                                            const std::vector<const Entry *> &b) {
		return a.front()->displayName.compareIgnoreCase(b.front()->displayName) < 0;
	});
	return groups;
}

bool Database::backupToZip(const juce::File &zipFile) const {
	if (!root.isDirectory()) return false;

	juce::ZipFile::Builder builder;
	for (const auto &entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFiles,
	                                                        juce::File::FollowSymlinks::no))
		builder.addFile(entry.getFile(), 0, entry.getFile().getRelativePathFrom(root));

	zipFile.deleteFile(); // createOutputStream() below would otherwise APPEND to a pre-existing file
	auto out = zipFile.createOutputStream();
	if (out == nullptr) return false;
	return builder.writeToStream(*out, nullptr);
}

bool Database::restoreFromZip(const juce::File &zipFile) {
	juce::ZipFile zip(zipFile);
	if (zip.getNumEntries() == 0) return false;

	bool hasIndex = false;
	for (int i = 0; i < zip.getNumEntries(); ++i)
		if (zip.getEntry(i)->filename == "index.json") hasIndex = true;
	if (!hasIndex) return false;

	// Extracted to a temp folder first and only swapped in on full success - a bad/corrupt zip
	// (or one that runs out of disk mid-extract) must leave the existing database untouched.
	const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
		                      .getChildFile("d110_soundbank_restore_" + juce::Uuid().toString().substring(0, 8));
	tempDir.createDirectory();
	const auto result = zip.uncompressTo(tempDir);
	if (result.failed()) {
		tempDir.deleteRecursively();
		return false;
	}

	root.deleteRecursively();
	tempDir.moveFileTo(root);
	load();
	return true;
}

juce::File Favorites::defaultFile() {
	return d110appdata::resolve("soundbank_favorites.json");
}

Favorites::Favorites(juce::File f) : indexFile(std::move(f)) { load(); }

void Favorites::load() {
	favouriteFiles.clear();
	if (!indexFile.existsAsFile()) return;
	const auto parsed = juce::JSON::parse(indexFile);
	if (const auto *arr = parsed.getArray())
		for (const auto &v : *arr) favouriteFiles.emplace_back(v.toString());
}

void Favorites::save() const {
	juce::Array<juce::var> arr;
	for (const auto &f : favouriteFiles) arr.add(f.getFullPathName());
	indexFile.getParentDirectory().createDirectory();
	indexFile.replaceWithText(juce::JSON::toString(juce::var(arr)));
}

bool Favorites::contains(const Entry &entry) const {
	for (const auto &f : favouriteFiles)
		if (f == entry.file) return true;
	return false;
}

void Favorites::toggle(const Entry &entry) {
	for (size_t i = 0; i < favouriteFiles.size(); ++i) {
		if (favouriteFiles[i] != entry.file) continue;
		favouriteFiles.erase(favouriteFiles.begin() + long(i));
		save();
		return;
	}
	favouriteFiles.push_back(entry.file);
	save();
}

} // namespace d110bank
