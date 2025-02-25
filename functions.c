/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>

#include "app/dtmf.h"
#if defined(ENABLE_FMRADIO)
	#include "app/fm.h"
#endif
#include "bsp/dp32g030/gpio.h"
#include "dcs.h"
#if defined(ENABLE_FMRADIO)
	#include "driver/bk1080.h"
#endif
#include "driver/bk4819.h"
#include "driver/gpio.h"
#include "driver/system.h"
#include "functions.h"
#include "helper/battery.h"
#include "misc.h"
#include "radio.h"
#include "settings.h"
#include "ui/status.h"
#include "ui/ui.h"

FUNCTION_Type_t gCurrentFunction;

void FUNCTION_Init(void)
{
	#ifdef ENABLE_NOAA
		if (IS_NOT_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE))
	#endif
	{
		gCurrentCodeType = gSelectedCodeType;
		if (gCssScanMode == CSS_SCAN_MODE_OFF)
			gCurrentCodeType = gRxVfo->AM_mode ? CODE_TYPE_OFF : gRxVfo->pRX->CodeType;
	}
	#ifdef ENABLE_NOAA
		else
			gCurrentCodeType = CODE_TYPE_CONTINUOUS_TONE;
	#endif

	DTMF_clear_RX();
	
	g_CxCSS_TAIL_Found = false;
	g_CDCSS_Lost       = false;
	g_CTCSS_Lost       = false;
					  
	g_VOX_Lost         = false;
	g_SquelchLost      = false;

	gFlagTailNoteEliminationComplete   = false;
	gTailNoteEliminationCountdown_10ms = 0;
	gFoundCTCSS                        = false;
	gFoundCDCSS                        = false;
	gFoundCTCSSCountdown_10ms          = 0;
	gFoundCDCSSCountdown_10ms          = 0;
	gEndOfRxDetectedMaybe              = false;

	#ifdef ENABLE_NOAA
		gNOAACountdown_10ms = 0;
	#endif
}

void FUNCTION_Select(FUNCTION_Type_t Function)
{
	const FUNCTION_Type_t PreviousFunction = gCurrentFunction;
	const bool            bWasPowerSave    = (PreviousFunction == FUNCTION_POWER_SAVE);

	gCurrentFunction = Function;

	if (bWasPowerSave && Function != FUNCTION_POWER_SAVE)
	{
		BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable();
		gRxIdleMode = false;
		UI_DisplayStatus(false);
	}

	switch (Function)
	{
		case FUNCTION_FOREGROUND:
			if (gDTMF_ReplyState != DTMF_REPLY_NONE)
				RADIO_PrepareCssTX();

			if (PreviousFunction == FUNCTION_TRANSMIT)
			{
				gVFO_RSSI_bar_level[0] = 0;
				gVFO_RSSI_bar_level[1] = 0;
			}
			else
			if (PreviousFunction != FUNCTION_RECEIVE)
				break;

			#if defined(ENABLE_FMRADIO)
				if (gFmRadioMode)
					gFM_RestoreCountdown_10ms = fm_restore_countdown_10ms;
			#endif

			if (gDTMF_CallState == DTMF_CALL_STATE_CALL_OUT || gDTMF_CallState == DTMF_CALL_STATE_RECEIVED)
				gDTMF_AUTO_RESET_TIME = 1 + (gEeprom.DTMF_AUTO_RESET_TIME * 2);

			return;
	
		case FUNCTION_MONITOR:
			gMonitor = true;
			break;
			
		case FUNCTION_INCOMING:
		case FUNCTION_RECEIVE:
			break;
	
		case FUNCTION_POWER_SAVE:
			gPowerSave_10ms            = gEeprom.BATTERY_SAVE * 10;
			gPowerSaveCountdownExpired = false;

			gRxIdleMode = true;
			
			gMonitor = false;

			BK4819_DisableVox();
			BK4819_Sleep();
			
			BK4819_ToggleGpioOut(BK4819_GPIO6_PIN2, false);

			gUpdateStatus = true;

			GUI_SelectNextDisplay(DISPLAY_MAIN);
			return;
	
		case FUNCTION_TRANSMIT:

			// if DTMF is enabled when TX'ing, it changes the TX audio filtering !! .. 1of11
			BK4819_DisableDTMF();

			// clear the DTMF RX buffer
			DTMF_clear_RX();

			// clear the DTMF RX live decoder buffer
			gDTMF_RX_live_timeout = 0;
			gDTMF_RX_live_timeout = 0;
			memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));
			
			#if defined(ENABLE_FMRADIO)
				if (gFmRadioMode)
					BK1080_Init(0, false);
			#endif

			#ifdef ENABLE_ALARM
				if (gAlarmState == ALARM_STATE_TXALARM && gEeprom.ALARM_MODE != ALARM_MODE_TONE)
				{
					gAlarmState = ALARM_STATE_ALARM;

					GUI_DisplayScreen();

					GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);

					SYSTEM_DelayMs(20);
					BK4819_PlayTone(500, 0);
					SYSTEM_DelayMs(2);

					GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);

					gEnableSpeaker = true;

					SYSTEM_DelayMs(60);
					BK4819_ExitTxMute();

					gAlarmToneCounter = 0;
					break;
				}
			#endif
			
			GUI_DisplayScreen();

			RADIO_SetTxParameters();

			// turn the RED LED on
			BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_RED, true);
	
			DTMF_Reply();
	
			#ifdef ENABLE_ALARM
				if (gAlarmState != ALARM_STATE_OFF)
				{
					BK4819_TransmitTone(true, (gAlarmState == ALARM_STATE_TX1750) ? 1750 : 500);
					SYSTEM_DelayMs(2);
					GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_AUDIO_PATH);
					gAlarmToneCounter = 0;
					gEnableSpeaker    = true;
					break;
				}
			#endif
		
			if (gCurrentVfo->SCRAMBLING_TYPE > 0 && gSetting_ScrambleEnable)
				BK4819_EnableScramble(gCurrentVfo->SCRAMBLING_TYPE - 1);
			else
				BK4819_DisableScramble();

			break;

		case FUNCTION_BAND_SCOPE:
			break;
	}

	gBatterySaveCountdown_10ms = battery_save_count_10ms;
	gSchedulePowerSave         = false;

	#if defined(ENABLE_FMRADIO)
		gFM_RestoreCountdown_10ms = 0;
	#endif
}
