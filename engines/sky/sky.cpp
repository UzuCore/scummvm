/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "backends/keymapper/keymapper.h"

#include "common/config-manager.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "common/file.h"
#include "common/md5.h"

#include "sky/control.h"
#include "sky/debug.h"
#include "sky/disk.h"
#include "sky/grid.h"
#include "sky/intro.h"
#include "sky/logic.h"
#include "sky/mouse.h"
#include "sky/music/adlibmusic.h"
#include "sky/music/gmmusic.h"
#include "sky/music/mt32music.h"
#include "sky/music/musicbase.h"
#include "sky/screen.h"
#include "sky/sky.h"
#include "sky/skydefs.h"
#include "sky/sound.h"
#include "sky/text.h"
#include "sky/compact.h"

#include "audio/mididrv.h"
#include "audio/mixer.h"

#include "engines/util.h"

/*
 At the beginning the reverse engineers were happy, and did rejoice at
 their task, for the engine before them did shineth and was full of
 promise. But then they did look closer and see'th the awful truth;
 its code was assembly and messy (rareth was its comments). And so large
 were its includes that did at first seem small; queereth also was its
 compact(s). Then they did findeth another version, and this was slightly
 different from the first. Then a third, and this was different again.
 All different, but not really better, for all were not really compatible.
 But, eventually, it did come to pass that Steel Sky was implemented on
 a modern platform. And the programmers looked and saw that it was indeed a
 miracle. But they were not joyous and instead did weep for nobody knew
 just what had been done. Except people who read the source. Hello.

 With apologies to the CD32 SteelSky file.
*/

namespace Sky {

void *SkyEngine::_itemList[300];
SystemVars *SkyEngine::_systemVars = nullptr;
const char *SkyEngine::shortcutsKeymapId = "sky-shortcuts";

SkyEngine::SkyEngine(OSystem *syst)
	: Engine(syst), _fastMode(0), _debugger(0), _big5Font(nullptr) {
	_systemVars = new SystemVars();
	_systemVars->systemFlags    = 0;
	_systemVars->gameVersion    = 0;
	_systemVars->mouseFlag      = 0;
	_systemVars->language       = 0;
	_systemVars->currentPalette = 4316;
	_systemVars->gameSpeed      = 0;
	_systemVars->currentMusic   = 0;
	_systemVars->pastIntro      = false;
	_systemVars->paused         = false;
	_systemVars->textDirRTL     = false;

	memset (_chineseTraditionalOffsets, 0, sizeof(_chineseTraditionalOffsets));
	_chineseTraditionalBlock = nullptr;

	_action     = kSkyActionNone;
	_skyLogic   = nullptr;
	_skySound   = nullptr;
	_skyMusic   = nullptr;
	_skyText    = nullptr;
	_skyMouse   = nullptr;
	_skyScreen  = nullptr;

	_skyDisk    = nullptr;
	_skyControl = nullptr;
	_skyCompact = nullptr;
}

SkyEngine::~SkyEngine() {
	delete _skyLogic;
	delete _skySound;
	delete _skyMusic;
	delete _skyText;
	delete _skyMouse;
	delete _skyScreen;
	//_debugger is deleted by Engine
	delete _skyDisk;
	delete _skyControl;
	delete _skyCompact;

	for (int i = 0; i < 300; i++)
		if (_itemList[i])
			free(_itemList[i]);

	delete _systemVars;
	delete [] _chineseTraditionalBlock;
	_chineseTraditionalBlock = nullptr;

	delete _big5Font;
	_big5Font = nullptr;
}

void SkyEngine::syncSoundSettings() {
	Engine::syncSoundSettings();

	bool mute = false;
	if (ConfMan.hasKey("mute"))
		mute = ConfMan.getBool("mute");

	if (ConfMan.getBool("sfx_mute")) // set mute sfx status for native options menu (F5)
		SkyEngine::_systemVars->systemFlags |= SF_FX_OFF;

	if (ConfMan.getBool("music_mute")) { // CD version allows to mute music from native options menu (F5)
		SkyEngine::_systemVars->systemFlags |= SF_MUS_OFF;
	}
	// SkyEngine native sound volume range is [0, 127]
	// However, via ScummVM UI, the volume range can be set within [0, 256]
	// so we "translate" between them
	_skyMusic->setVolume(mute ? 0: CLIP(ConfMan.getInt("music_volume") >> 1, 0, 127));

	// write-back to ini file for persistence
	ConfMan.flushToDisk();
}

void SkyEngine::initVirgin() {
	_skyScreen->setPalette(60111);
	_skyScreen->showScreen(60110);
}

void SkyEngine::handleKey() {
	if ((_action != kSkyActionNone || _keyPressed.keycode) && _systemVars->paused) {
		_skySound->fnUnPauseFx();
		_systemVars->paused = false;
		_skyScreen->setPaletteEndian((uint8 *)_skyCompact->fetchCpt(SkyEngine::_systemVars->currentPalette));
	} else {
		switch (_action) {
		case kSkyActionToggleFastMode:
			_fastMode ^= 1;
			break;

		case kSkyActionToggleReallyFastMode:
			_fastMode ^= 2;
			break;

		case kSkyActionOpenControlPanel:
			_skyControl->doControlPanel();
			break;

		case kSkyActionSkip:
			if (!_systemVars->pastIntro)
				_skyControl->restartGame();
			break;

		case kSkyActionSkipLine:
			_skyMouse->logicClick();
			break;

		case kSkyActionPause:
			_skyScreen->halvePalette();
			_skySound->fnPauseFx();
			_systemVars->paused = true;
			break;

		default:
			break;
		}
	}

	_action = kSkyActionNone;
	_keyPressed.reset();
}

Common::Error SkyEngine::go() {
	_action = kSkyActionNone;

	uint16 result = 0;
	if (ConfMan.hasKey("save_slot")) {
		int saveSlot = ConfMan.getInt("save_slot");
		if (saveSlot >= 0 && saveSlot <= MAX_SAVE_GAMES)
			result = _skyControl->quickXRestore(ConfMan.getInt("save_slot"));
	}

	if (result != GAME_RESTORED) {
		bool introSkipped = false;
		// Clear pastIntro here (set to false) explicilty
		// It should be false already, but better to ensure it
		_systemVars->pastIntro = false;
		if (_systemVars->gameVersion > 272) { // don't do intro for floppydemos
			Intro *skyIntro = new Intro(_skyDisk, _skyScreen, _skyMusic, _skySound, _skyText, _mixer, _system);
			bool floppyIntro = ConfMan.getBool("alt_intro");
			introSkipped = !skyIntro->doIntro(floppyIntro);
			delete skyIntro;
		}

		if (!shouldQuit()) {
			_skyScreen->clearScreen(true);
			// restartGame() takes us to the first scene, without showing the
			// initial animation where Foster is being chased. initScreen0()
			// shows the first scene together with that animation. We can't
			// call both, as they both load the same scene.
			if (introSkipped) {
				// restart game sets the _systemVars->pastIntro = true;
				_skyControl->restartGame();
			} else {

				_skyLogic->initScreen0();
			}
		}
	}

	uint32 delayCount = _system->getMillis();
	while (!shouldQuit()) {
		_skySound->checkFxQueue();
		_skyMouse->mouseEngine();
		handleKey();
		if (_systemVars->paused) {
			do {
				_system->updateScreen();
				delay(50);
				handleKey();
			} while (_systemVars->paused);
			delayCount = _system->getMillis();
		}

		_skyLogic->engine();
		_skyScreen->processSequence();
		_skyScreen->recreate();
		_skyScreen->spriteEngine();
		if (_debugger->showGrid()) {
			uint8 *grid = _skyLogic->_skyGrid->giveGrid(Logic::_scriptVariables[SCREEN]);
			if (grid) {
				_skyScreen->showGrid(grid);
				_skyScreen->forceRefresh();
			}
		}
		_skyScreen->flip();

		if (_fastMode & 2)
			delay(0);
		else if (_fastMode & 1)
			delay(10);
		else {
			delayCount += _systemVars->gameSpeed;
			int needDelay = delayCount - (int)_system->getMillis();
			if ((needDelay < 0) || (needDelay > _systemVars->gameSpeed)) {
				needDelay = 0;
				delayCount = _system->getMillis();
			}
			delay(needDelay);
		}
	}

	_skyControl->showGameQuitMsg();
	_skyMusic->stopMusic();
	ConfMan.flushToDisk();
	delay(1500);
	return Common::kNoError;
}

static const struct {
	// Identification
	const char *md5; // File MD5
	uint length; // File length
	// Main section
	// Offset from the beginning of file to virtual address 0
	uint32 virtualBase;
	// Offset to index of string sections
	uint stringSectionIndexOffset;
	// Offset to the font
	uint fontOffset;
	// Next one isn't strictly necessary but makes logic simpler
	// by allowing to read string block into memory as whole
	// without any parsing. Just has to cover the block containing
	// the strings. Reading more (up to whole file) is OK.
	// End of strings block.
	uint stringBlockEnd;
} chineseExes[] = {
	{
		// Identification
		"7bc128ba9bfaecb9bb4ef328b756057a", 575538,
		// Main
		0x5191, 0x6427e, 0x54afc,
		// Value to simplify code
		0x7eee1
	}
};

bool SkyEngine::loadChineseTraditional() {
	Common::File skyExe;
	if (!skyExe.open("sky.exe"))
		return false;
	uint length = skyExe.size();
	Common::String md5 = Common::computeStreamMD5AsString(skyExe, length);

	for (uint i = 0; i < ARRAYSIZE(chineseExes); i++) {
		if (md5 == chineseExes[i].md5 && length == chineseExes[i].length) {
			skyExe.seek(chineseExes[i].stringSectionIndexOffset);
			for (uint j = 0; j < 8; j++)
				_chineseTraditionalOffsets[j] = skyExe.readUint32LE() + chineseExes[i].virtualBase;
			uint32 stringBlockOffset = _chineseTraditionalOffsets[0];
			for (uint j = 1; j < 8; j++)
				stringBlockOffset = MIN(_chineseTraditionalOffsets[j], stringBlockOffset);
			for (uint j = 0; j < 8; j++)
				_chineseTraditionalOffsets[j] -= stringBlockOffset;
			uint stringBlockLen = chineseExes[i].stringBlockEnd - stringBlockOffset;
			_chineseTraditionalBlock = new char[stringBlockLen];
			skyExe.seek(stringBlockOffset);
			skyExe.read(_chineseTraditionalBlock, stringBlockLen);

			skyExe.seek(chineseExes[i].fontOffset);
			_big5Font = new Graphics::Big5Font();
			_big5Font->loadPrefixedRaw(skyExe, 15);
			return true;
		}
	}

	return false;
}

Common::Error SkyEngine::init() {
	initGraphics(320, 200);

	_skyDisk = new Disk();
	_skySound = new Sound(_mixer, _skyDisk, Audio::Mixer::kMaxChannelVolume);

	_systemVars->gameVersion = _skyDisk->determineGameVersion();

	MidiDriver::DeviceHandle dev = MidiDriver::detectDevice(MDT_ADLIB | MDT_MIDI | MDT_PREFER_MT32);
	if (MidiDriver::getMusicType(dev) == MT_ADLIB) {
		_systemVars->systemFlags |= SF_SBLASTER;
		_skyMusic = new AdLibMusic(_mixer, _skyDisk);
	} else {
		_systemVars->systemFlags |= SF_ROLAND;
		if ((MidiDriver::getMusicType(dev) == MT_MT32) || ConfMan.getBool("native_mt32"))
			_skyMusic = new MT32Music(MidiDriver::createMidi(dev), _mixer, _skyDisk);
		else
			_skyMusic = new GmMusic(MidiDriver::createMidi(dev), _mixer, _skyDisk);
	}

	if (isCDVersion()) {
		if (ConfMan.hasKey("nosubtitles")) {
			warning("Configuration key 'nosubtitles' is deprecated. Use 'subtitles' instead");
			if (!ConfMan.getBool("nosubtitles"))
				_systemVars->systemFlags |= SF_ALLOW_TEXT;
		}

		if (ConfMan.getBool("subtitles"))
			_systemVars->systemFlags |= SF_ALLOW_TEXT;

		if (!ConfMan.getBool("speech_mute"))
			_systemVars->systemFlags |= SF_ALLOW_SPEECH;

	} else
		_systemVars->systemFlags |= SF_ALLOW_TEXT;

	_systemVars->systemFlags |= SF_PLAY_VOCS;
	_systemVars->gameSpeed = 80;

	_skyCompact = new SkyCompact();
	_skyText = new Text(this, _skyDisk, _skyCompact);
	_skyMouse = new Mouse(_system, _skyDisk, _skyCompact);
	_skyScreen = new Screen(_system, _skyDisk, _skyCompact);

	initVirgin();
	initItemList();
	loadFixedItems();
	_skyLogic = new Logic(_skyCompact, _skyScreen, _skyDisk, _skyText, _skyMusic, _skyMouse, _skySound);
	_skyMouse->useLogicInstance(_skyLogic);

	Common::Keymapper *keymapper = _system->getEventManager()->getKeymapper();
	Common::Keymap *shortcutsKeymap = keymapper->getKeymap(shortcutsKeymapId);
	assert(shortcutsKeymap);

	_skyControl = new Control(this, _saveFileMan, _skyScreen, _skyDisk, _skyMouse, _skyText, _skyMusic, _skyLogic, _skySound, _skyCompact, _system, shortcutsKeymap);
	_skyLogic->useControlInstance(_skyControl);

	switch (Common::parseLanguage(ConfMan.get("language"))) {
	case Common::EN_USA:
		_systemVars->language = SKY_USA;
		break;
	case Common::DE_DEU:
		_systemVars->language = SKY_GERMAN;
		break;
	case Common::FR_FRA:
		_systemVars->language = SKY_FRENCH;
		break;
	case Common::IT_ITA:
		_systemVars->language = SKY_ITALIAN;
		break;
	case Common::PT_BRA:
		_systemVars->language = SKY_PORTUGUESE;
		break;
	case Common::ES_ESP:
		_systemVars->language = SKY_SPANISH;
		break;
	case Common::SV_SWE:
		_systemVars->language = SKY_SWEDISH;
		break;
	case Common::EN_GRB:
		_systemVars->language = SKY_ENGLISH;
		break;
	case Common::ZH_TWN:
		_systemVars->language = SKY_CHINESE_TRADITIONAL;
		break;
	case Common::HE_ISR:
		_systemVars->textDirRTL = true;
		break;

	default:
		_systemVars->language = SKY_ENGLISH;
		break;
	}

	if (_systemVars->language == SKY_CHINESE_TRADITIONAL && !loadChineseTraditional()) {
		_systemVars->language = SKY_ENGLISH;
	}

	if (_systemVars->language != SKY_CHINESE_TRADITIONAL &&
	    !_skyDisk->fileExists(60600 + SkyEngine::_systemVars->language * 8)) {
		warning("The language you selected does not exist in your BASS version");
		if (_skyDisk->fileExists(60600))
			SkyEngine::_systemVars->language = SKY_ENGLISH; // default to GB english if it exists..
		else if (_skyDisk->fileExists(60600 + SKY_USA * 8))
			SkyEngine::_systemVars->language = SKY_USA;		// try US english...
		else
			for (uint8 cnt = SKY_ENGLISH; cnt <= SKY_SPANISH; cnt++)
				if (_skyDisk->fileExists(60600 + cnt * 8)) {	// pick the first language we can find
					SkyEngine::_systemVars->language = cnt;
					break;
				}
	}

	// Setup mixer: Default volume is set to 192 (ScummVM)
	//              which is 96 (in-game) volume
	ConfMan.registerDefault("sfx_volume",    192);
	ConfMan.registerDefault("music_volume",  192);
	ConfMan.registerDefault("speech_volume", 192);
	ConfMan.registerDefault("mute", "false");
	syncSoundSettings();

	_debugger = new Debugger(_skyLogic, _skyMouse, _skyScreen, _skyCompact);
	setDebugger(_debugger);
	return Common::kNoError;
}

void SkyEngine::initItemList() {
	//See List.asm for (cryptic) item # descriptions

	for (int i = 0; i < 300; i++)
		_itemList[i] = NULL;
}

void SkyEngine::loadFixedItems() {
	_itemList[49] = _skyDisk->loadFile(49);
	_itemList[50] = _skyDisk->loadFile(50);
	_itemList[73] = _skyDisk->loadFile(73);
	_itemList[262] = _skyDisk->loadFile(262);

	if (!isDemo()) {
		_itemList[36] = _skyDisk->loadFile(36);
		_itemList[263] = _skyDisk->loadFile(263);
		_itemList[264] = _skyDisk->loadFile(264);
		_itemList[265] = _skyDisk->loadFile(265);
		_itemList[266] = _skyDisk->loadFile(266);
		_itemList[267] = _skyDisk->loadFile(267);
		_itemList[269] = _skyDisk->loadFile(269);
		_itemList[271] = _skyDisk->loadFile(271);
		_itemList[272] = _skyDisk->loadFile(272);
	}
}

void *SkyEngine::fetchItem(uint32 num) {
	return _itemList[num];
}

void SkyEngine::delay(int32 amount) {
	Common::Event event;

	uint32 start = _system->getMillis();
	_action = kSkyActionNone;
	_keyPressed.reset();

	if (amount < 0)
		amount = 0;

	do {
		while (_eventMan->pollEvent(event)) {
			switch (event.type) {
			case Common::EVENT_CUSTOM_ENGINE_ACTION_START:
				_action = (SkyAction)event.customType;
				break;
			case Common::EVENT_KEYDOWN:
				_keyPressed = event.kbd;
				break;
			case Common::EVENT_MOUSEMOVE:
				if (!(_systemVars->systemFlags & SF_MOUSE_LOCKED))
					_skyMouse->mouseMoved(event.mouse.x, event.mouse.y);
				break;
			case Common::EVENT_LBUTTONDOWN:
				if (!(_systemVars->systemFlags & SF_MOUSE_LOCKED))
					_skyMouse->mouseMoved(event.mouse.x, event.mouse.y);
				_skyMouse->buttonPressed(2);
				break;
			case Common::EVENT_RBUTTONDOWN:
				if (!(_systemVars->systemFlags & SF_MOUSE_LOCKED))
					_skyMouse->mouseMoved(event.mouse.x, event.mouse.y);
				_skyMouse->buttonPressed(1);
				break;
			default:
				break;
			}
		}

		_system->updateScreen();

		if (amount > 0)
			_system->delayMillis((amount > 10) ? 10 : amount);

	} while (_system->getMillis() < start + amount);
}

bool SkyEngine::isDemo() {
	switch (_systemVars->gameVersion) {
	case 109: // PC Gamer demo
	case 267: // English floppy demo
	case 272: // German floppy demo
	case 365: // CD demo
		return true;
	case 288:
	case 303:
	case 331:
	case 348:
	case 368:
	case 372:
		return false;
	default:
		error("Unknown game version %d", _systemVars->gameVersion);
	}
}

bool SkyEngine::isCDVersion() {
	switch (_systemVars->gameVersion) {
	case 109:
	case 267:
	case 272:
	case 288:
	case 303:
	case 331:
	case 348:
		return false;
	case 365:
	case 368:
	case 372:
		return true;
	default:
		error("Unknown game version %d", _systemVars->gameVersion);
	}
}

} // End of namespace Sky
