#include "io/midi/ai_sysex.h"
#include "definitions_cxx.hpp"
#include "fatfs/ff.h"
#include "gui/ui/load/load_instrument_preset_ui.h"
#include "gui/ui/load/load_song_ui.h"
#include "gui/ui/ui.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/session_view.h"
#include "io/midi/midi_device.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/sample/sample.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patch_cable_set.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"

namespace {

enum Command : uint8_t {
	PING = 0,
	SET_CURRENT_NOTE = 1,
	CLEAR_CURRENT_CLIP = 2,
	SELECT_SESSION_ROW = 3,
	SET_ROW_EXPRESSION = 4,
	SET_CURRENT_CLIP_LENGTH = 5,
	SET_CLIP_PARAM_AUTOMATION = 6,
	SELECT_SESSION_INDEX = 7,
	CLONE_SESSION_INDEX = 8,
	CREATE_SESSION_INDEX = 9,
	CLAMP_SESSION_INDEX_LENGTH = 10,
	SET_CURRENT_CLIP_NAME = 11,
	LOAD_CURRENT_SYNTH_PRESET = 12,
	SYNC_SHARED_OUTPUT_PARAMS = 13,
	CLONE_SESSION_VISIBLE = 14,
	CREATE_SESSION_VISIBLE = 15,
	CLAMP_SESSION_VISIBLE_LENGTH = 16,
	SET_SECTION_REPEAT = 17,
	SET_SIDECHAIN = 18,
	SAVE_SONG = 19,
	LOAD_SONG = 20,
};

enum Status : uint8_t {
	OK = 0,
	BAD_LENGTH = 1,
	NO_SONG = 2,
	NO_INSTRUMENT_CLIP = 3,
	NOTE_ROW_FAILED = 4,
	ACTION_FAILED = 5,
	ADD_FAILED = 6,
	CLEAR_FAILED = 7,
	NO_KIT_ROW = 8,
	NO_CLIP = 9,
	BAD_PARAM = 10,
	SD_CARD_UNAVAILABLE = 11,
	FILE_NOT_FOUND = 12,
	FILE_EXISTS = 13,
	BUSY = 14,
	NEEDS_UI_SAVE = 15,
};

uint16_t read14(uint8_t msb, uint8_t lsb) {
	return ((uint16_t)(msb & 0x7f) << 7) | (lsb & 0x7f);
}

int32_t read21(uint8_t b0, uint8_t b1, uint8_t b2) {
	return ((int32_t)(b0 & 0x7f) << 14) | ((int32_t)(b1 & 0x7f) << 7) | (b2 & 0x7f);
}

int32_t value14ToBig(int32_t value14) {
	return (value14 - 8192) << 18;
}

int32_t visibleClipIndexToInternal(int32_t visibleIndex) {
	int32_t numClips = currentSong->sessionClips.getNumElements();
	return numClips - 1 - visibleIndex;
}

int32_t visibleInsertIndexToInternal(int32_t visibleIndex) {
	int32_t numClips = currentSong->sessionClips.getNumElements();
	if (visibleIndex < 0) {
		visibleIndex = 0;
	}
	else if (visibleIndex > numClips) {
		visibleIndex = numClips;
	}
	return numClips - visibleIndex;
}

void sendReply(MIDICable& cable, uint8_t command, uint8_t status) {
	const uint8_t reply[] = {SysEx::SYSEX_START,
	                         SysEx::DELUGE_SYSEX_ID_BYTE0,
	                         SysEx::DELUGE_SYSEX_ID_BYTE1,
	                         SysEx::DELUGE_SYSEX_ID_BYTE2,
	                         SysEx::DELUGE_SYSEX_ID_BYTE3,
	                         SysEx::SysexCommands::AI,
	                         command,
	                         status,
	                         SysEx::SYSEX_END};
	cable.sendSysex(reply, sizeof(reply));
}

void write21(uint8_t* out, int32_t value) {
	if (value < 0) {
		value = 0;
	}
	if (value > 0x1fffff) {
		value = 0x1fffff;
	}
	out[0] = (value >> 14) & 0x7f;
	out[1] = (value >> 7) & 0x7f;
	out[2] = value & 0x7f;
}

void sendLengthReply(MIDICable& cable, uint8_t command, uint8_t status, int32_t index, Clip* clip) {
	int32_t loopLength = 0;
	int32_t maxLength = 0;
	int32_t maxIndependentRowLength = 0;
	int32_t numRows = 0;
	if (clip) {
		loopLength = clip->loopLength;
		maxLength = clip->getMaxLength();
		if (clip->type == ClipType::INSTRUMENT) {
			InstrumentClip* instrumentClip = (InstrumentClip*)clip;
			numRows = instrumentClip->noteRows.getNumElements();
			for (int32_t i = 0; i < numRows; i++) {
				NoteRow* noteRow = instrumentClip->noteRows.getElement(i);
				if (noteRow->loopLengthIfIndependent > maxIndependentRowLength) {
					maxIndependentRowLength = noteRow->loopLengthIfIndependent;
				}
			}
		}
	}
	uint8_t reply[] = {SysEx::SYSEX_START,
	                   SysEx::DELUGE_SYSEX_ID_BYTE0,
	                   SysEx::DELUGE_SYSEX_ID_BYTE1,
	                   SysEx::DELUGE_SYSEX_ID_BYTE2,
	                   SysEx::DELUGE_SYSEX_ID_BYTE3,
	                   SysEx::SysexCommands::AI,
	                   command,
	                   status,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   0,
	                   SysEx::SYSEX_END};
	write21(&reply[8], index);
	write21(&reply[11], loopLength);
	write21(&reply[14], maxLength);
	write21(&reply[17], maxIndependentRowLength);
	reply[20] = numRows & 0x7f;
	cable.sendSysex(reply, sizeof(reply));
}

void forceClipLength(Clip* clip, int32_t newLength) {
	int32_t oldLength = clip->loopLength;
	clip->loopLength = newLength;
	clip->originalLength = newLength;
	if (clip->type == ClipType::INSTRUMENT) {
		InstrumentClip* instrumentClip = (InstrumentClip*)clip;
		for (int32_t i = 0; i < instrumentClip->noteRows.getNumElements(); i++) {
			NoteRow* noteRow = instrumentClip->noteRows.getElement(i);
			noteRow->loopLengthIfIndependent = 0;
		}
	}
	clip->output->clipLengthChanged(clip, oldLength);
	clip->expectEvent();
}

Status clampSessionIndexLength(uint8_t* data, int32_t len, Clip** outClip, int32_t* outIndex) {
	// Payload after the AI namespace command:
	//   0: subcommand = CLAMP_SESSION_INDEX_LENGTH
	//   1-2: absolute session clip index, 14-bit
	//   3-5: new loop length, 21-bit
	if (outClip) {
		*outClip = nullptr;
	}
	if (outIndex) {
		*outIndex = 0;
	}
	if (len < 6) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	int32_t index = read14(data[1], data[2]);
	if (outIndex) {
		*outIndex = index;
	}
	if (index < 0 || index >= currentSong->sessionClips.getNumElements()) {
		return NO_CLIP;
	}
	int32_t newLength = read21(data[3], data[4], data[5]);
	if (newLength <= 0) {
		return BAD_LENGTH;
	}
	Clip* clip = currentSong->sessionClips.getClipAtIndex(index);
	if (!clip) {
		return NO_CLIP;
	}
	forceClipLength(clip, newLength);
	currentSong->setCurrentClip(clip);
	if (outClip) {
		*outClip = clip;
	}
	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}
	return OK;
}

Status clampSessionVisibleLength(uint8_t* data, int32_t len, Clip** outClip, int32_t* outIndex) {
	// Payload matches CLAMP_SESSION_INDEX_LENGTH, but index is a visible song row from top-to-bottom.
	if (outClip) {
		*outClip = nullptr;
	}
	if (outIndex) {
		*outIndex = 0;
	}
	if (len < 6) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	int32_t visibleIndex = read14(data[1], data[2]);
	int32_t index = visibleClipIndexToInternal(visibleIndex);
	if (outIndex) {
		*outIndex = visibleIndex;
	}
	if (index < 0 || index >= currentSong->sessionClips.getNumElements()) {
		return NO_CLIP;
	}
	int32_t newLength = read21(data[3], data[4], data[5]);
	if (newLength <= 0) {
		return BAD_LENGTH;
	}
	Clip* clip = currentSong->sessionClips.getClipAtIndex(index);
	if (!clip) {
		return NO_CLIP;
	}
	forceClipLength(clip, newLength);
	currentSong->setCurrentClip(clip);
	if (outClip) {
		*outClip = clip;
	}
	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}
	return OK;
}

Status setSectionRepeat(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_SECTION_REPEAT
	//   1: section colour id
	//   2-3: repeat count encoded as signed14 + 8192. 0=infinite, 1=once then next.
	//   4: flags bit0 = enable AI section loop
	//   5: optional loop start section
	//   6: optional loop end section
	if (len < 4) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	uint8_t section = data[1] & 0x7f;
	if (section >= kMaxNumSections) {
		return BAD_PARAM;
	}
	int32_t repetitions = (int32_t)read14(data[2], data[3]) - 8192;
	if (repetitions < LAUNCH_EXCLUSIVE || repetitions > 9999) {
		return BAD_PARAM;
	}
	currentSong->sections[section].numRepetitions = repetitions;

	if (len >= 5) {
		bool loopSections = (data[4] & 1) != 0;
		uint8_t loopStart = len >= 6 ? (data[5] & 0x7f) : section;
		uint8_t loopEnd = len >= 7 ? (data[6] & 0x7f) : section;
		if (loopSections && (loopStart >= kMaxNumSections || loopEnd >= kMaxNumSections)) {
			return BAD_PARAM;
		}
		currentSong->aiLoopSections = loopSections;
		currentSong->aiLoopSectionStart = loopStart;
		currentSong->aiLoopSectionEnd = loopEnd;
		currentSong->sectionToReturnToAfterSongEnd = loopSections ? loopStart : REALLY_OUT_OF_RANGE;
	}

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status setCurrentClipName(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_CURRENT_CLIP_NAME
	//   1: target flags, bit 0 = clip name, bit 1 = output / instrument name
	//   2..n: ASCII / 7-bit name bytes
	if (len < 3) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	Clip* clip = currentSong->getCurrentClip();
	if (!clip) {
		return NO_CLIP;
	}

	uint8_t flags = data[1] & 0x7f;
	if ((flags & 0x03) == 0) {
		return BAD_PARAM;
	}

	char name[33];
	int32_t nameLength = std::min<int32_t>(len - 2, 32);
	for (int32_t i = 0; i < nameLength; i++) {
		uint8_t c = data[i + 2] & 0x7f;
		name[i] = (c >= 32 && c <= 126) ? (char)c : ' ';
	}
	name[nameLength] = 0;

	Error error = Error::NONE;
	if (flags & 0x01) {
		error = clip->name.set(name);
		if (error != Error::NONE) {
			return ACTION_FAILED;
		}
	}
	if ((flags & 0x02) && clip->output) {
		error = clip->output->name.set(name);
		if (error != Error::NONE) {
			return ACTION_FAILED;
		}
	}

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}
	return OK;
}

Status loadCurrentSynthPreset(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = LOAD_CURRENT_SYNTH_PRESET
	//   1..n: ASCII preset name from SYNTHS, with or without .XML
	if (len < 2) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (!clip) {
		return NO_INSTRUMENT_CLIP;
	}

	char presetNameChars[65];
	int32_t nameLength = std::min<int32_t>(len - 1, 64);
	for (int32_t i = 0; i < nameLength; i++) {
		uint8_t c = data[i + 1] & 0x7f;
		presetNameChars[i] = (c >= 32 && c <= 126) ? (char)c : ' ';
	}
	presetNameChars[nameLength] = 0;

	char filename[69];
	int32_t extensionStart = nameLength - 4;
	bool hasXmlExtension =
	    extensionStart >= 0 && (presetNameChars[extensionStart] == '.')
	    && (presetNameChars[extensionStart + 1] == 'X' || presetNameChars[extensionStart + 1] == 'x')
	    && (presetNameChars[extensionStart + 2] == 'M' || presetNameChars[extensionStart + 2] == 'm')
	    && (presetNameChars[extensionStart + 3] == 'L' || presetNameChars[extensionStart + 3] == 'l');
	if (hasXmlExtension) {
		snprintf(filename, sizeof(filename), "%s", presetNameChars);
		presetNameChars[extensionStart] = 0;
	}
	else {
		snprintf(filename, sizeof(filename), "%s.XML", presetNameChars);
	}

	Error sdError = StorageManager::initSD();
	if (sdError != Error::NONE) {
		return SD_CARD_UNAVAILABLE;
	}

	Error error = Browser::currentDir.set("SYNTHS");
	if (error != Error::NONE) {
		return ACTION_FAILED;
	}
	String searchName;
	error = searchName.set(filename);
	if (error != Error::NONE) {
		return ACTION_FAILED;
	}
	auto presetResult =
	    loadInstrumentPresetUI.confirmPresetOrNextUnlaunchedOne(OutputType::SYNTH, &searchName, Availability::ANY);
	if (!presetResult) {
		return FILE_NOT_FOUND;
	}
	FileItem* presetFile = presetResult.value();

	String presetName;
	String dirPath;
	error = presetName.set(presetNameChars);
	if (error == Error::NONE) {
		dirPath.set(&Browser::currentDir);
	}
	if (error != Error::NONE) {
		Browser::emptyFileItems();
		return ACTION_FAILED;
	}

	Instrument* newInstrument = nullptr;
	error = StorageManager::loadInstrumentFromFile(currentSong, clip, OutputType::SYNTH, false, &newInstrument,
	                                               &presetFile->filePointer, &presetName, &dirPath);
	Browser::emptyFileItems();
	if (error != Error::NONE || !newInstrument) {
		return ACTION_FAILED;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	error = clip->changeInstrument(modelStack, newInstrument, nullptr, InstrumentRemoval::DELETE_OR_HIBERNATE_IF_UNUSED,
	                               nullptr, true);
	if (error != Error::NONE) {
		return ACTION_FAILED;
	}
	currentSong->addOutput(newInstrument);
	clip->output->setActiveClip(modelStack);

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}
	return OK;
}

Status syncSharedOutputParams(uint8_t* data, int32_t len) {
	// kept for protocol compatibility; hot-path group removed
	(void)data;
	(void)len;
	return OK;
}

Status setCurrentNote(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_CURRENT_NOTE
	//   1: MIDI note / pitch
	//   2-3: position, 14-bit big endian 7-bit bytes
	//   4-5: length, 14-bit big endian 7-bit bytes
	//   6: velocity
	// Extended timing form:
	//   2-4: position, 21-bit big endian 7-bit bytes
	//   5-7: length, 21-bit big endian 7-bit bytes
	//   8: velocity
	if (len < 7) {
		return BAD_LENGTH;
	}

	if (!currentSong) {
		return NO_SONG;
	}

	InstrumentClip* clip = getCurrentInstrumentClip();
	if (!clip) {
		return NO_INSTRUMENT_CLIP;
	}

	int32_t yNote = data[1] & 0x7f;
	bool wideTiming = len >= 9;
	int32_t pos = wideTiming ? read21(data[2], data[3], data[4]) : read14(data[2], data[3]);
	int32_t length = wideTiming ? read21(data[5], data[6], data[7]) : read14(data[4], data[5]);
	int32_t velocity = wideTiming ? (data[8] & 0x7f) : (data[6] & 0x7f);

	if (length <= 0) {
		length = 1;
	}
	if (velocity <= 0) {
		velocity = 1;
	}
	if (pos < 0 || pos >= clip->loopLength) {
		return BAD_PARAM;
	}
	if (pos + length > clip->loopLength) {
		length = clip->loopLength - pos;
	}
	if (length <= 0) {
		return BAD_LENGTH;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);

	ModelStackWithNoteRow* modelStackWithNoteRow = nullptr;
	if (clip->output->type == OutputType::KIT) {
		// Match MIDI Follow's default kit mapping: C1 / note 36 is the bottom kit row,
		// and each semitone above moves one kit row up.
		int32_t yDisplay = yNote - 36;
		modelStackWithNoteRow = clip->getNoteRowOnScreen(yDisplay, modelStack);
	}
	else {
		bool scaleAltered = false;
		modelStackWithNoteRow = clip->getOrCreateNoteRowForYNote(yNote, modelStack, nullptr, &scaleAltered);
	}
	if (!modelStackWithNoteRow) {
		return NOTE_ROW_FAILED;
	}

	NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
	if (!noteRow) {
		return clip->output->type == OutputType::KIT ? NO_KIT_ROW : NOTE_ROW_FAILED;
	}

	int32_t probability = noteRow->getDefaultProbability();
	Iterance iterance = noteRow->getDefaultIterance();
	int32_t fill = noteRow->getDefaultFill(modelStackWithNoteRow);

	if (!noteRow->attemptNoteAdd(pos, length, velocity, probability, iterance, fill, modelStackWithNoteRow, nullptr)) {
		return ADD_FAILED;
	}

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status clearCurrentClip(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = CLEAR_CURRENT_CLIP
	//   1-2: start position, 14-bit big endian 7-bit bytes
	//   3-4: end position, 14-bit big endian 7-bit bytes
	// Extended timing form:
	//   1-3: start position, 21-bit big endian 7-bit bytes
	//   4-6: end position, 21-bit big endian 7-bit bytes
	if (len < 5) {
		return BAD_LENGTH;
	}

	if (!currentSong) {
		return NO_SONG;
	}

	InstrumentClip* clip = getCurrentInstrumentClip();
	if (!clip) {
		return NO_INSTRUMENT_CLIP;
	}

	bool wideTiming = len >= 7;
	int32_t startPos = wideTiming ? read21(data[1], data[2], data[3]) : read14(data[1], data[2]);
	int32_t endPos = wideTiming ? read21(data[4], data[5], data[6]) : read14(data[3], data[4]);
	if (endPos <= startPos) {
		return BAD_LENGTH;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);

	int32_t loopLength = clip->loopLength;
	if (startPos == 0 && endPos >= loopLength) {
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			NoteRow* noteRow = clip->noteRows.getElement(i);
			int32_t noteRowId = clip->getNoteRowId(noteRow, i);
			ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(noteRowId, noteRow);
			noteRow->clear(nullptr, modelStackWithNoteRow, false, true);
		}
		clip->expectEvent();
	}
	else {
		clip->clearArea(modelStack, startPos, endPos, nullptr);
	}

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status selectSessionRow(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SELECT_SESSION_ROW
	//   1: visible session row / yDisplay, 0 is top row
	//   2: optional OutputType to create if the row is empty; 127 means don't create
	if (len < 2) {
		return BAD_LENGTH;
	}

	if (!currentSong) {
		return NO_SONG;
	}

	int32_t rowFromTop = data[1] & 0x7f;
	if (rowFromTop >= kDisplayHeight) {
		return BAD_LENGTH;
	}

	int32_t yDisplay = (kDisplayHeight - 1) - rowFromTop;
	int32_t clipIndex = yDisplay + currentSong->songViewYScroll;
	OutputType createType = OutputType::NONE;
	if (len >= 3 && data[2] != 127) {
		if (data[2] > (uint8_t)OutputType::AUDIO) {
			return BAD_PARAM;
		}
		createType = (OutputType)data[2];
	}

	Clip* clip = nullptr;
	if (clipIndex < 0 || clipIndex >= currentSong->sessionClips.getNumElements()) {
		if (createType == OutputType::NONE) {
			return NO_CLIP;
		}
		clip = sessionView.createNewClip(createType, yDisplay);
	}
	else {
		clip = currentSong->sessionClips.getClipAtIndex(clipIndex);
		if (!clip && createType != OutputType::NONE) {
			clip = sessionView.createNewClip(createType, yDisplay);
		}
	}
	if (!clip) {
		return createType == OutputType::NONE ? NO_CLIP : ACTION_FAILED;
	}

	sessionView.selectedClipYDisplay = yDisplay;
	sessionView.selectedClipPressYDisplay = yDisplay;
	sessionView.selectedClipPressXDisplay = 0;
	sessionView.performActionOnPadRelease = false;
	currentSong->setCurrentClip(clip);

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status selectSessionIndex(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SELECT_SESSION_INDEX
	//   1-2: absolute session clip index, 14-bit big endian 7-bit bytes
	if (len < 3) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}

	int32_t clipIndex = read14(data[1], data[2]);
	if (clipIndex < 0 || clipIndex >= currentSong->sessionClips.getNumElements()) {
		return NO_CLIP;
	}

	Clip* clip = currentSong->sessionClips.getClipAtIndex(clipIndex);
	if (!clip) {
		return NO_CLIP;
	}

	currentSong->setCurrentClip(clip);
	int32_t yDisplay = clipIndex - currentSong->songViewYScroll;
	if (yDisplay < 0) {
		currentSong->songViewYScroll += yDisplay;
		yDisplay = 0;
	}
	else if (yDisplay >= kDisplayHeight) {
		currentSong->songViewYScroll += yDisplay - kDisplayHeight + 1;
		yDisplay = kDisplayHeight - 1;
	}
	sessionView.selectedClipYDisplay = yDisplay;
	sessionView.selectedClipPressYDisplay = yDisplay;
	sessionView.selectedClipPressXDisplay = 0;
	sessionView.performActionOnPadRelease = false;

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status cloneSessionInternal(int32_t sourceIndex, int32_t destIndex, uint8_t section, bool hasSection,
                            int32_t loopLength) {
	int32_t numClips = currentSong->sessionClips.getNumElements();
	if (sourceIndex < 0 || sourceIndex >= numClips) {
		return NO_CLIP;
	}
	if (destIndex < 0) {
		destIndex = 0;
	}
	if (destIndex > numClips) {
		destIndex = numClips;
	}

	Clip* sourceClip = currentSong->sessionClips.getClipAtIndex(sourceIndex);
	if (!sourceClip) {
		return NO_CLIP;
	}
	if (!currentSong->sessionClips.ensureEnoughSpaceAllocated(1)) {
		return ACTION_FAILED;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(sourceClip);
	Error error = sourceClip->clone(modelStack);
	if (error != Error::NONE) {
		return ACTION_FAILED;
	}

	Clip* newClip = (Clip*)modelStack->getTimelineCounter();
	newClip->section = hasSection ? section : sourceClip->section;
	newClip->activeIfNoSolo = false;
	if (currentSong->sessionClips.insertClipAtIndex(newClip, destIndex) != Error::NONE) {
		newClip->~Clip();
		delugeDealloc(newClip);
		return ACTION_FAILED;
	}

	currentSong->setCurrentClip(newClip);
	if (loopLength > 0) {
		if (loopLength <= 0) {
			return BAD_LENGTH;
		}
		forceClipLength(newClip, loopLength);
	}
	int32_t yDisplay = destIndex - currentSong->songViewYScroll;
	if (yDisplay < 0) {
		currentSong->songViewYScroll += yDisplay;
		yDisplay = 0;
	}
	else if (yDisplay >= kDisplayHeight) {
		currentSong->songViewYScroll += yDisplay - kDisplayHeight + 1;
		yDisplay = kDisplayHeight - 1;
	}
	sessionView.selectedClipYDisplay = yDisplay;
	sessionView.selectedClipPressYDisplay = yDisplay;
	sessionView.selectedClipPressXDisplay = 0;
	sessionView.performActionOnPadRelease = false;

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status cloneSessionIndex(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = CLONE_SESSION_INDEX
	//   1-2: source absolute session clip index, 14-bit
	//   3-4: destination insert index, 14-bit. Values past the end append.
	//   5: optional session section / colour group.
	//   6-8: optional loop length, 21-bit.
	if (len < 5) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}

	int32_t loopLength = len >= 8 ? (len >= 9 ? read21(data[6], data[7], data[8]) : read14(data[6], data[7])) : 0;
	uint8_t section = len >= 6 ? (data[5] & 0x7f) : 0;
	return cloneSessionInternal(read14(data[1], data[2]), read14(data[3], data[4]), section, len >= 6, loopLength);
}

Status cloneSessionVisible(uint8_t* data, int32_t len) {
	// Source and destination are visible song rows, counted top-to-bottom.
	if (len < 5) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}

	int32_t sourceIndex = visibleClipIndexToInternal(read14(data[1], data[2]));
	int32_t destIndex = visibleInsertIndexToInternal(read14(data[3], data[4]));
	int32_t loopLength = len >= 8 ? (len >= 9 ? read21(data[6], data[7], data[8]) : read14(data[6], data[7])) : 0;
	uint8_t section = len >= 6 ? (data[5] & 0x7f) : 0;
	return cloneSessionInternal(sourceIndex, destIndex, section, len >= 6, loopLength);
}

Status createBlankSynthSessionIndex(int32_t destIndex, uint8_t section, int32_t loopLength) {
	if (!currentSong->sessionClips.ensureEnoughSpaceAllocated(1)) {
		return ACTION_FAILED;
	}

	void* clipMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(InstrumentClip));
	if (!clipMemory) {
		return ACTION_FAILED;
	}
	InstrumentClip* clip = new (clipMemory) InstrumentClip(currentSong);

	ParamManagerForTimeline paramManager;
	Instrument* instrument = StorageManager::createNewInstrument(OutputType::SYNTH, &paramManager);
	if (!instrument) {
		clip->~InstrumentClip();
		delugeDealloc(clipMemory);
		return ACTION_FAILED;
	}

	Error error = instrument->dirPath.set("SYNTHS");
	if (error == Error::NONE) {
		error = instrument->name.set("AI");
	}
	if (error != Error::NONE) {
		void* instrumentMemory = dynamic_cast<void*>(instrument);
		instrument->~Instrument();
		delugeDealloc(instrumentMemory);
		clip->~InstrumentClip();
		delugeDealloc(clipMemory);
		return ACTION_FAILED;
	}

	((SoundInstrument*)instrument)->setupAsDefaultSynth(&paramManager);
	clip->colourOffset = 0;
	clip->soloingInSessionMode = false;
	clip->wasActiveBefore = false;
	clip->isPendingOverdub = false;
	clip->isUnfinishedAutoOverdub = false;
	clip->armState = ArmState::OFF;
	clip->loopLength = loopLength > 0 ? loopLength : currentSong->getBarLength();
	clip->originalLength = clip->loopLength;

	error = clip->setAudioInstrument(instrument, currentSong, true, &paramManager);
	if (error != Error::NONE) {
		void* instrumentMemory = dynamic_cast<void*>(instrument);
		instrument->~Instrument();
		delugeDealloc(instrumentMemory);
		clip->~InstrumentClip();
		delugeDealloc(clipMemory);
		return ACTION_FAILED;
	}
	currentSong->addOutput(instrument);

	clip->section = section;
	clip->activeIfNoSolo = false;
	if (currentSong->sessionClips.insertClipAtIndex(clip, destIndex) != Error::NONE) {
		clip->~InstrumentClip();
		delugeDealloc(clipMemory);
		return ACTION_FAILED;
	}

	currentSong->setCurrentClip(clip);
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	clip->output->setActiveClip(modelStack);

	return OK;
}

Status createSessionIndex(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = CREATE_SESSION_INDEX
	//   1-2: destination insert index, 14-bit. Values past the end append.
	//   3: OutputType: synth=0, kit=1, midi=2, cv=3, audio=4
	//   4: session section / colour group.
	//   5-7: optional loop length, 21-bit.
	if (len < 5) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}

	int32_t destIndex = read14(data[1], data[2]);
	if (destIndex < 0) {
		destIndex = 0;
	}
	if (destIndex > currentSong->sessionClips.getNumElements()) {
		destIndex = currentSong->sessionClips.getNumElements();
	}
	if (data[3] > (uint8_t)OutputType::AUDIO) {
		return BAD_PARAM;
	}
	OutputType createType = (OutputType)data[3];
	uint8_t section = data[4] & 0x7f;
	int32_t loopLength = 0;
	if (len >= 7) {
		loopLength = len >= 8 ? read21(data[5], data[6], data[7]) : read14(data[5], data[6]);
		if (loopLength <= 0) {
			return BAD_LENGTH;
		}
	}

	currentSong->songViewYScroll = destIndex;
	Clip* clip = sessionView.createNewClip(createType, 0);
	if (!clip) {
		return ACTION_FAILED;
	}
	clip->section = section;
	clip->activeIfNoSolo = false;
	if (loopLength > 0) {
		forceClipLength(clip, loopLength);
	}
	currentSong->setCurrentClip(clip);

	int32_t yDisplay = destIndex - currentSong->songViewYScroll;
	if (yDisplay < 0) {
		currentSong->songViewYScroll += yDisplay;
		yDisplay = 0;
	}
	else if (yDisplay >= kDisplayHeight) {
		currentSong->songViewYScroll += yDisplay - kDisplayHeight + 1;
		yDisplay = kDisplayHeight - 1;
	}
	sessionView.selectedClipYDisplay = yDisplay;
	sessionView.selectedClipPressYDisplay = yDisplay;
	sessionView.selectedClipPressXDisplay = 0;
	sessionView.performActionOnPadRelease = false;

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status setRowExpression(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_ROW_EXPRESSION
	//   1: MIDI note / kit row note
	//   2: expression dimension: 0 pitch, 1 timbre/mod wheel, 2 pressure
	//   3-4: start position, 14-bit big endian 7-bit bytes
	//   5-6: length, 14-bit big endian 7-bit bytes
	//   7-8: value, 14-bit unsigned. For pitch, 8192 is centre.
	// Extended timing form:
	//   3-5: start position, 21-bit big endian 7-bit bytes
	//   6-8: length, 21-bit big endian 7-bit bytes
	//   9-10: value, 14-bit unsigned.
	if (len < 9) {
		return BAD_LENGTH;
	}

	if (!currentSong) {
		return NO_SONG;
	}

	InstrumentClip* clip = getCurrentInstrumentClip();
	if (!clip) {
		return NO_INSTRUMENT_CLIP;
	}

	int32_t yNote = data[1] & 0x7f;
	int32_t dimension = data[2] & 0x7f;
	if (dimension >= kNumExpressionDimensions) {
		return BAD_PARAM;
	}

	bool wideTiming = len >= 11;
	int32_t pos = wideTiming ? read21(data[3], data[4], data[5]) : read14(data[3], data[4]);
	int32_t length = wideTiming ? read21(data[6], data[7], data[8]) : read14(data[5], data[6]);
	int32_t value14 = wideTiming ? read14(data[9], data[10]) : read14(data[7], data[8]);
	if (length <= 0) {
		return BAD_LENGTH;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);

	ModelStackWithNoteRow* modelStackWithNoteRow = nullptr;
	if (clip->output->type == OutputType::KIT) {
		int32_t yDisplay = yNote - 36;
		modelStackWithNoteRow = clip->getNoteRowOnScreen(yDisplay, modelStack);
	}
	else {
		bool scaleAltered = false;
		modelStackWithNoteRow = clip->getOrCreateNoteRowForYNote(yNote, modelStack, nullptr, &scaleAltered);
	}
	if (!modelStackWithNoteRow) {
		return NOTE_ROW_FAILED;
	}

	NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
	if (!noteRow) {
		return clip->output->type == OutputType::KIT ? NO_KIT_ROW : NOTE_ROW_FAILED;
	}

	noteRow->paramManager.ensureExpressionParamSetExists(clip->output->type == OutputType::KIT);
	ParamCollectionSummary* summary = noteRow->paramManager.getExpressionParamSetSummary();
	ExpressionParamSet* expressionParams = (ExpressionParamSet*)summary->paramCollection;
	if (!expressionParams) {
		return ACTION_FAILED;
	}

	AutoParam* param = &expressionParams->params[dimension];
	ModelStackWithAutoParam* modelStackWithAutoParam =
	    modelStackWithNoteRow->addOtherTwoThingsAutomaticallyGivenNoteRow()->addParam(expressionParams, summary,
	                                                                                  dimension, param);

	param->setValueForRegion(pos, length, value14ToBig(value14), modelStackWithAutoParam);

	clip->reGetParameterAutomation(modelStack);
	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status createSessionVisible(uint8_t* data, int32_t len) {
	// Destination is a visible song row, counted top-to-bottom.
	if (len < 5) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}

	int32_t destIndex = visibleInsertIndexToInternal(read14(data[1], data[2]));
	if (data[3] > (uint8_t)OutputType::AUDIO) {
		return BAD_PARAM;
	}
	OutputType createType = (OutputType)data[3];
	uint8_t section = data[4] & 0x7f;
	int32_t loopLength = 0;
	if (len >= 7) {
		loopLength = len >= 8 ? read21(data[5], data[6], data[7]) : read14(data[5], data[6]);
		if (loopLength <= 0) {
			return BAD_LENGTH;
		}
	}

	currentSong->songViewYScroll = destIndex;
	Clip* clip = sessionView.createNewClip(createType, 0);
	if (!clip) {
		return ACTION_FAILED;
	}
	clip->section = section;
	clip->activeIfNoSolo = false;
	if (loopLength > 0) {
		forceClipLength(clip, loopLength);
	}
	currentSong->setCurrentClip(clip);

	int32_t yDisplay = destIndex - currentSong->songViewYScroll;
	if (yDisplay < 0) {
		currentSong->songViewYScroll += yDisplay;
		yDisplay = 0;
	}
	else if (yDisplay >= kDisplayHeight) {
		currentSong->songViewYScroll += yDisplay - kDisplayHeight + 1;
		yDisplay = kDisplayHeight - 1;
	}
	sessionView.selectedClipYDisplay = yDisplay;
	sessionView.selectedClipPressYDisplay = yDisplay;
	sessionView.selectedClipPressXDisplay = 0;
	sessionView.performActionOnPadRelease = false;

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status setClipParamAutomation(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_CLIP_PARAM_AUTOMATION
	//   1: param alias: 0 LPF freq, 1 LPF resonance, 2 HPF freq, 3 reverb amount
	//   2: shape: 0 region / hold start value, 1 ramp start->end
	//   3-4: start position, 14-bit big endian 7-bit bytes
	//   5-6: length, 14-bit big endian 7-bit bytes
	//   7-8: start value, 14-bit unsigned. 0 low, 16383 high.
	//   9-10: end value, 14-bit unsigned. Used for ramp shape.
	// Extended timing form:
	//   3-5: start position, 21-bit big endian 7-bit bytes
	//   6-8: length, 21-bit big endian 7-bit bytes
	//   9-10: start value, 14-bit unsigned.
	//   11-12: end value, 14-bit unsigned.
	if (len < 11) {
		return BAD_LENGTH;
	}

	if (!currentSong) {
		return NO_SONG;
	}

	Clip* clip = currentSong->getCurrentClip();
	if (!clip || !clip->output) {
		return NO_CLIP;
	}
	if (clip->output->type != OutputType::SYNTH) {
		return BAD_PARAM;
	}

	uint8_t alias = data[1] & 0x7f;
	uint8_t shape = data[2] & 0x7f;
	bool wideTiming = len >= 13;
	int32_t pos = wideTiming ? read21(data[3], data[4], data[5]) : read14(data[3], data[4]);
	int32_t length = wideTiming ? read21(data[6], data[7], data[8]) : read14(data[5], data[6]);
	int32_t startValue = value14ToBig(wideTiming ? read14(data[9], data[10]) : read14(data[7], data[8]));
	int32_t endValue = value14ToBig(wideTiming ? read14(data[11], data[12]) : read14(data[9], data[10]));
	if (length <= 0) {
		return BAD_LENGTH;
	}

	using namespace deluge::modulation;
	params::Kind paramKind = params::Kind::PATCHED;
	int32_t paramID = params::LOCAL_LPF_FREQ;
	switch (alias) {
	case 0:
		paramID = params::LOCAL_LPF_FREQ;
		break;
	case 1:
		paramID = params::LOCAL_LPF_RESONANCE;
		break;
	case 2:
		paramID = params::LOCAL_HPF_FREQ;
		break;
	case 3:
		paramID = params::GLOBAL_REVERB_AMOUNT;
		break;
	default:
		return BAD_PARAM;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	ModelStackWithAutoParam* modelStackWithAutoParam =
	    clip->output->getModelStackWithParam(modelStack, clip, paramID, paramKind, true, false);
	if (!modelStackWithAutoParam || !modelStackWithAutoParam->autoParam || !modelStackWithAutoParam->paramCollection
	    || !modelStackWithAutoParam->summary) {
		return BAD_PARAM;
	}

	AutoParam* param = modelStackWithAutoParam->autoParam;
	if (shape == 0) {
		param->setValueForRegion(pos, length, startValue, modelStackWithAutoParam);
	}
	else if (shape == 1) {
		int32_t oldValue = param->getCurrentValue();
		bool automatedBefore = param->isAutomated();
		int32_t endPos = pos + length;
		if (clip->loopLength > 0 && endPos >= clip->loopLength) {
			endPos = clip->loopLength - 1;
		}
		param->deleteNodesWithinRegion(modelStackWithAutoParam, pos, length);
		if (param->setNodeAtPos(pos, startValue, true) == -1 || param->setNodeAtPos(endPos, endValue, true) == -1) {
			return ACTION_FAILED;
		}
		modelStackWithAutoParam->paramCollection->notifyParamModifiedInSomeWay(modelStackWithAutoParam, oldValue, true,
		                                                                       automatedBefore, param->isAutomated());
	}
	else {
		return BAD_PARAM;
	}

	clip->reGetParameterAutomation(modelStack);
	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status setCurrentClipLength(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_CURRENT_CLIP_LENGTH
	//   1-2: new loop length, 14-bit big endian 7-bit bytes
	// Extended timing form:
	//   1-3: new loop length, 21-bit big endian 7-bit bytes
	if (len < 3) {
		return BAD_LENGTH;
	}

	if (!currentSong) {
		return NO_SONG;
	}

	Clip* clip = currentSong->getCurrentClip();
	if (!clip) {
		return NO_CLIP;
	}

	int32_t newLength = len >= 4 ? read21(data[1], data[2], data[3]) : read14(data[1], data[2]);
	if (newLength <= 0) {
		return BAD_LENGTH;
	}

	forceClipLength(clip, newLength);

	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}

	return OK;
}

Status setSidechain(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SET_SIDECHAIN
	//   1: mode, 0 = kit row send, 1 = current clip/sound volume duck receive
	//   mode 0:
	//     2: MIDI note. Note 36 is the bottom visible kit row, as in AI note writing.
	//     3-4: send amount, 14-bit unsigned. 0 off, 16383 max.
	//   mode 1:
	//     2-3: receive amount, 14-bit unsigned. 8192 neutral, 0 deep duck, 16383 boost.
	if (len < 4) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (!clip || !clip->output) {
		return NO_INSTRUMENT_CLIP;
	}

	uint8_t mode = data[1] & 0x7f;
	if (mode == 0) {
		if (len < 5) {
			return BAD_LENGTH;
		}
		if (clip->output->type != OutputType::KIT) {
			return BAD_PARAM;
		}
		int32_t yDisplay = (data[2] & 0x7f) - 36;
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
		ModelStackWithNoteRow* modelStackWithNoteRow = clip->getNoteRowOnScreen(yDisplay, modelStack);
		if (!modelStackWithNoteRow) {
			return NO_KIT_ROW;
		}
		NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
		if (!noteRow || !noteRow->drum || noteRow->drum->type != DrumType::SOUND) {
			return NO_KIT_ROW;
		}
		int32_t amount14 = read14(data[3], data[4]);
		int32_t amount = amount14 >= 0x3fff ? 2147483647 : amount14 * 131080;
		((SoundDrum*)noteRow->drum)->sideChainSendLevel = amount;
	}
	else if (mode == 1) {
		int32_t amount = value14ToBig(read14(data[2], data[3]));
		using namespace deluge::modulation;

		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack =
		    setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addOtherTwoThingsButNoNoteRow(clip->output->toModControllable(), &clip->paramManager);

		if (clip->paramManager.containsAnyMainParamCollections()) {
			clip->paramManager.getUnpatchedParamSet()
			    ->params[params::UNPATCHED_SIDECHAIN_VOLUME]
			    .setCurrentValueBasicForSetup(amount);
		}

		if (clip->output->type == OutputType::SYNTH) {
			PatchCableSet* patchCableSet = clip->paramManager.getPatchCableSet();
			ParamDescriptor destination;
			destination.setToHaveParamOnly(params::GLOBAL_VOLUME_POST_REVERB_SEND);
			ModelStackWithParamCollection* modelStackWithPatchCableSet =
			    clip->paramManager.getPatchCableSet(modelStackWithThreeMainThings);
			uint8_t cableIndex = patchCableSet->getPatchCableIndex(PatchSource::SIDECHAIN, destination,
			                                                       modelStackWithPatchCableSet, true);
			if (cableIndex == 255) {
				return ACTION_FAILED;
			}
			patchCableSet->patchCables[cableIndex].param.setCurrentValueBasicForSetup(amount);
		}
		AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;
	}
	else {
		return BAD_PARAM;
	}

	clip->expectEvent();
	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}
	return OK;
}

// Song names over SysEx: 7-bit ASCII, letters/digits/space/-/_ kept, anything
// else becomes '_'. Returns false for an empty or too-long name.
bool readSongName(uint8_t* data, int32_t len, int32_t from, char* out, int32_t outSize) {
	int32_t n = 0;
	for (int32_t i = from; i < len && data[i] != 0; i++) {
		if (n >= outSize - 1) {
			return false;
		}
		char c = (char)(data[i] & 0x7f);
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-'
		          || c == '_';
		out[n++] = ok ? c : '_';
	}
	out[n] = 0;
	return n > 0;
}

Status songPath(String& path, char const* name) {
	if (path.set("SONGS/") != Error::NONE || path.concatenate(name) != Error::NONE
	    || path.concatenate(".XML") != Error::NONE) {
		return ACTION_FAILED;
	}
	return OK;
}

Status saveSong(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = SAVE_SONG
	//   1: flags, bit 0 = may overwrite an existing song of that name
	//   2..: song name, 7-bit ASCII (saved as SONGS/<name>.XML)
	// The headless core of SaveSongUI::performSave: the song file is written
	// with the firmware's own serializer, to a temp file first when it would
	// overwrite. Songs whose samples would need copying (an unsaved recording,
	// or samples loaded from a "collected" song folder) are refused with
	// NEEDS_UI_SAVE - save those once from the Deluge's own Save menu.
	if (len < 3) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	bool mayOverwrite = data[1] & 0x01;
	char name[64];
	if (!readSongName(data, len, 2, name, sizeof(name))) {
		return BAD_PARAM;
	}
	if (currentSong->hasAnyPendingNextOverdubs()) {
		return BUSY;
	}
	for (int32_t i = 0; i < audioFileManager.audioFiles.getNumElements(); i++) {
		AudioFile* audioFile = (AudioFile*)audioFileManager.audioFiles.getElement(i);
		if (!audioFile->numReasonsToBeLoaded) {
			continue;
		}
		if (!audioFile->loadedFromAlternatePath.isEmpty()) {
			return NEEDS_UI_SAVE;
		}
		if (audioFile->type == AudioFileType::SAMPLE && !((Sample*)audioFile)->tempFilePathForRecording.isEmpty()) {
			return NEEDS_UI_SAVE;
		}
	}

	String filePath;
	if (songPath(filePath, name) != OK) {
		return ACTION_FAILED;
	}
	bool existed = StorageManager::fileExists(filePath.get());
	if (existed && !mayOverwrite) {
		return FILE_EXISTS;
	}

	String writePath;
	if (existed) {
		for (int32_t n = 0;; n++) {
			if (writePath.set("SONGS/TEMP") != Error::NONE || writePath.concatenateInt(n, 4) != Error::NONE
			    || writePath.concatenate(".XML") != Error::NONE) {
				return ACTION_FAILED;
			}
			if (!StorageManager::fileExists(writePath.get())) {
				break;
			}
		}
	}
	else {
		writePath.set(&filePath);
	}

	if (StorageManager::createXMLFile(writePath.get(), smSerializer, false, false) != Error::NONE) {
		return SD_CARD_UNAVAILABLE;
	}
	currentSong->writeToFile();
	if (GetSerializer().closeFileAfterWriting(writePath.get(), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<song\n",
	                                          "\n</song>\n")
	    != Error::NONE) {
		return ACTION_FAILED;
	}
	if (existed) {
		if (f_unlink(filePath.get()) != FR_OK || f_rename(writePath.get(), filePath.get()) != FR_OK) {
			return ACTION_FAILED;
		}
	}

	currentSong->name.set(name);
	currentSong->dirPath.set("SONGS");
	if (getRootUI()) {
		uiNeedsRendering(getRootUI(), 0xFFFFFFFF, 0);
	}
	return OK;
}

Status loadSong(uint8_t* data, int32_t len) {
	// Payload after the AI namespace command:
	//   0: subcommand = LOAD_SONG
	//   1..: song name, 7-bit ASCII (loads SONGS/<name>.XML)
	// The same path the firmware takes for its startup song: point the song
	// at the file, open the load UI and perform the load. Refused with BUSY
	// while the transport runs (that path loads two songs at once) or while
	// a menu or browser is open - stop playback and return to a main view.
	if (len < 2) {
		return BAD_LENGTH;
	}
	if (!currentSong) {
		return NO_SONG;
	}
	char name[64];
	if (!readSongName(data, len, 1, name, sizeof(name))) {
		return BAD_PARAM;
	}
	String filePath;
	if (songPath(filePath, name) != OK) {
		return ACTION_FAILED;
	}
	if (!StorageManager::fileExists(filePath.get())) {
		return FILE_NOT_FOUND;
	}
	if (playbackHandler.isEitherClockActive() || getCurrentUI() != getRootUI()) {
		return BUSY;
	}

	currentSong->setSongFullPath(filePath.get());
	if (!openUI(&loadSongUI)) {
		return ACTION_FAILED;
	}
	loadSongUI.performLoad();

	String wanted;
	wanted.set(name);
	if (!currentSong || !currentSong->name.equalsCaseIrrespective(&wanted)) {
		return ACTION_FAILED;
	}
	return OK;
}

} // namespace

void AISysex::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 1) {
		return;
	}

	uint8_t command = data[0];
	Status status = BAD_LENGTH;

	switch (command) {
	case PING:
		status = OK;
		break;
	case SET_CURRENT_NOTE:
		status = setCurrentNote(data, len);
		break;
	case CLEAR_CURRENT_CLIP:
		status = clearCurrentClip(data, len);
		break;
	case SELECT_SESSION_ROW:
		status = selectSessionRow(data, len);
		break;
	case SET_ROW_EXPRESSION:
		status = setRowExpression(data, len);
		break;
	case SET_CURRENT_CLIP_LENGTH:
		status = setCurrentClipLength(data, len);
		break;
	case SET_CLIP_PARAM_AUTOMATION:
		status = setClipParamAutomation(data, len);
		break;
	case SELECT_SESSION_INDEX:
		status = selectSessionIndex(data, len);
		break;
	case CLONE_SESSION_INDEX:
		status = cloneSessionIndex(data, len);
		break;
	case CREATE_SESSION_INDEX:
		status = createSessionIndex(data, len);
		break;
	case CLAMP_SESSION_INDEX_LENGTH: {
		Clip* clip = nullptr;
		int32_t index = 0;
		status = clampSessionIndexLength(data, len, &clip, &index);
		sendLengthReply(cable, command, status, index, clip);
		return;
	}
	case SET_CURRENT_CLIP_NAME:
		status = setCurrentClipName(data, len);
		break;
	case LOAD_CURRENT_SYNTH_PRESET:
		status = loadCurrentSynthPreset(data, len);
		break;
	case SYNC_SHARED_OUTPUT_PARAMS:
		status = syncSharedOutputParams(data, len);
		break;
	case CLONE_SESSION_VISIBLE:
		status = cloneSessionVisible(data, len);
		break;
	case CREATE_SESSION_VISIBLE:
		status = createSessionVisible(data, len);
		break;
	case CLAMP_SESSION_VISIBLE_LENGTH: {
		Clip* clip = nullptr;
		int32_t index = 0;
		status = clampSessionVisibleLength(data, len, &clip, &index);
		sendLengthReply(cable, command, status, index, clip);
		return;
	}
	case SET_SECTION_REPEAT:
		status = setSectionRepeat(data, len);
		break;
	case SET_SIDECHAIN:
		status = setSidechain(data, len);
		break;
	case SAVE_SONG:
		status = saveSong(data, len);
		break;
	case LOAD_SONG:
		status = loadSong(data, len);
		break;
	default:
		status = BAD_LENGTH;
		break;
	}

	sendReply(cable, command, status);
}
