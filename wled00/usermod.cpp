#include "wled.h"
#include "audio_reactive.h"
/*
 * This v1 usermod file allows you to add own functionality to WLED more easily
 * See: https://github.com/Aircoookie/WLED/wiki/Add-own-functionality
 * EEPROM bytes 2750+ are reserved for your custom use case. (if you extend #define EEPSIZE in const.h)
 * If you just need 8 bytes, use 2551-2559 (you do not need to increase EEPSIZE)
 *
 * Consider the v2 usermod API if you need a more advanced feature set!
 */

/*
 * Functions and variable delarations moved to audio_reactive.h
 * Not 100% sure this was done right. There is probably a better way to handle this...
 */

// This gets called once at boot. Do all initialization that doesn't depend on network here
void userSetup() {
  disableSoundProcessing = true; // just to be safe
  // Reset I2S peripheral for good measure
  i2s_driver_uninstall(I2S_NUM_0);
  periph_module_reset(PERIPH_I2S0_MODULE);

  delay(100);         // Give that poor microphone some time to setup.
  switch (dmType) {
    case 1:
      Serial.print("AS: Generic I2S Microphone - "); Serial.println(I2S_MIC_CHANNEL_TEXT);
      audioSource = new I2SSource(SAMPLE_RATE, BLOCK_SIZE, 0, 0xFFFFFFFF);
      break;
    case 2:
      Serial.print("AS: ES7243 Microphone - "); Serial.println(I2S_MIC_CHANNEL_TEXT);
      audioSource = new ES7243(SAMPLE_RATE, BLOCK_SIZE, 0, 0xFFFFFFFF);
      break;
    case 3:
      Serial.print("AS: SPH0645 Microphone - "); Serial.println(I2S_MIC_CHANNEL_TEXT);
      audioSource = new SPH0654(SAMPLE_RATE, BLOCK_SIZE, 0, 0xFFFFFFFF);
      break;
    case 4:
      Serial.print("AS: Generic I2S Microphone with Master Clock - "); Serial.println(I2S_MIC_CHANNEL_TEXT);
      audioSource = new I2SSourceWithMasterClock(SAMPLE_RATE, BLOCK_SIZE, 0, 0xFFFFFFFF);
      break;
    case 5:
      Serial.print("AS: I2S PDM Microphone - "); Serial.println(I2S_MIC_CHANNEL_TEXT);
      audioSource = new I2SPdmSource(SAMPLE_RATE, BLOCK_SIZE, 0, 0xFFFFFFFF);
      break;
    case 0:
    default:
      Serial.println("AS: Analog Microphone (left channel only).");
      audioSource = new I2SAdcSource(SAMPLE_RATE, BLOCK_SIZE, 0, 0x0FFF);
      break;
  }

  delay(100);

  audioSource->initialize();
  delay(250);

  pinMode(LED_BUILTIN, OUTPUT);

  sampling_period_us = round(1000000*(1.0/SAMPLE_RATE));

  // Define the FFT Task and lock it to core 0
  xTaskCreatePinnedToCore(
        FFTcode,                          // Function to implement the task
        "FFT",                            // Name of the task
        5000,                            // Stack size in words
        NULL,                             // Task input parameter
        1,                                // Priority of the task
        &FFT_Task,                        // Task handle
        0);                               // Core where the task should run

  disableSoundProcessing = false; // let it run
}

// This gets called every time WiFi is (re-)connected. Initialize own network interfaces here
void userConnected() {
}

// userLoop. You can use "if (WLED_CONNECTED)" to check for successful connection
void userLoop() {

  // suspend local sound processing when "real time mode" is active (E131, UDP, ADALIGHT, ARTNET)
  if (  (realtimeOverride == REALTIME_OVERRIDE_NONE)  // user override
      &&(useMainSegmentOnly == false)                 // cannot suspend when "main segment only" is set - other segments may still need sound data.
      &&( (realtimeMode == REALTIME_MODE_GENERIC)     // these realtime modes take complete control of all LEDs, so it's safe to disable sound processing
        ||(realtimeMode == REALTIME_MODE_E131)
        ||(realtimeMode == REALTIME_MODE_UDP)
        ||(realtimeMode == REALTIME_MODE_ADALIGHT)
        ||(realtimeMode == REALTIME_MODE_ARTNET) ) ) 
  {
    #ifdef WLED_DEBUG
    if ((disableSoundProcessing == false) && (audioSyncEnabled == 0)) {  // we just switched to "disabled"
      DEBUG_PRINTLN("[AS userLoop] realtime mode active - audio processing suspended.");
      DEBUG_PRINTF( "              RealtimeMode = %d; RealtimeOverride = %d useMainSegmentOnly=%d\n", int(realtimeMode), int(realtimeOverride), int(useMainSegmentOnly));
    }
    #endif
    disableSoundProcessing = true;
  } else {
    #ifdef WLED_DEBUG
    if ((disableSoundProcessing == true) && (audioSyncEnabled == 0)) {    // we just switched to "disabled"
      DEBUG_PRINTLN("[AS userLoop] realtime mode ended - audio processing resumed.");
      DEBUG_PRINTF( "              RealtimeMode = %d; RealtimeOverride = %d useMainSegmentOnly=%d\n", int(realtimeMode), int(realtimeOverride), int(useMainSegmentOnly));
    }
    #endif
    if ((disableSoundProcessing == true) && (audioSyncEnabled == 0)) lastTime = millis();  // just left "realtime mode" - update timekeeping
    disableSoundProcessing = false;
  }

  if (audioSyncEnabled & (1 << 1)) disableSoundProcessing = true;   // make sure everything is disabled IF in audio Receive mode
  if (audioSyncEnabled & (1 << 0)) disableSoundProcessing = false;  // keep running audio IF we're in audio Transmit mode

  #ifdef WLED_DEBUG //(to surpress warning)
  int userloopDelay = int(millis() - lastTime);
  if (lastTime == 0) userloopDelay=0; // startup - don't have valid data from last run.
  #endif

  if ((!disableSoundProcessing) && (!(audioSyncEnabled & (1 << 1)))) { // Only run the sampling code IF we're not in realtime mode and not in audio Receive mode
    #ifdef WLED_DEBUG
    // compain when audio userloop has been delayed for long. Currently we need userloop running between 500 and 1500 times per second. 
    if (userloopDelay > 23) {    // should not happen. Expect lagging in SR effects if you see this mesage !!!
      DEBUG_PRINTF("[AS userLoop] hickup detected -> was inactive for last %d millis!\n", int(millis() - lastTime));
    }
    #endif

    lastTime = millis();
    if (soundAgc > AGC_NUM_PRESETS) soundAgc = 0; // make sure that AGC preset is valid (to avoid array bounds violation)
    getSample();                        // Sample the microphone
    agcAvg();                           // Calculated the PI adjusted value as sampleAvg
    myVals[millis()%32] = sampleAgc;

    static uint8_t lastMode = 0;
    static bool agcEffect = false;
    uint8_t knownMode = strip.getMainSegment().mode;

    if (lastMode != knownMode) { // only execute if mode changes
      char lineBuffer[3];
      /* uint8_t printedChars = */ extractModeName(knownMode, JSON_mode_names, lineBuffer, 3); //is this 'the' way to get mode name here?

      //used the following code to reverse engineer this
      // Serial.println(lineBuffer);
      // for (uint8_t i = 0; i<printedChars; i++) {
      //   Serial.print(i);
      //   Serial.print( ": ");
      //   Serial.println(uint8_t(lineBuffer[i]));
      // }
      agcEffect = (lineBuffer[1] == 226 && lineBuffer[2] == 153); // && (lineBuffer[3] == 170 || lineBuffer[3] == 171 ) encoding of ♪ or ♫
      // agcEffect = (lineBuffer[4] == 240 && lineBuffer[5] == 159 && lineBuffer[6] == 142 && lineBuffer[7] == 154 ); //encoding of 🎚 No clue why as not found here https://www.iemoji.com/view/emoji/918/objects/level-slider

      // if (agcEffect)
      //   Serial.println("found ♪ or ♫");
    }

    // update inputLevel Slider based on current AGC gain
    if ((soundAgc>0) && agcEffect) {
      static unsigned long last_update_time = 0;
      static unsigned long last_kick_time = 0;
      static int last_user_inputLevel = 0;
      unsigned long now_time = millis();    

      // "user kick" feature - if user has moved the slider by at least 32 units, we "kick" AGC gain by 30% (up or down)
      // only once in 3.5 seconds
      if (   (lastMode == knownMode)
          && (abs(last_user_inputLevel - inputLevel) > 31) 
          && (now_time - last_kick_time > 3500)) {
        if (last_user_inputLevel > inputLevel) multAgc *= 0.60; // down -> reduce gain
        if (last_user_inputLevel < inputLevel) multAgc *= 1.50; // up -> increase gain
        last_kick_time = now_time;
      }

      int new_user_inputLevel = 128.0 * multAgc;                                       // scale AGC multiplier so that "1" is at 128
      if (multAgc > 1.0) new_user_inputLevel = 128.0 * (((multAgc - 1.0) / 4.0) +1.0); // compress range so we can show values up to 4
      new_user_inputLevel = MIN(MAX(new_user_inputLevel, 0),255);

	    // update user interfaces - restrict frequency to avoid flooding UI's with small changes
      if ( ( ((now_time - last_update_time > 3500) && (abs(new_user_inputLevel - inputLevel) > 2))     // small change - every 3.5 sec (max) 
           ||((now_time - last_update_time > 2200) && (abs(new_user_inputLevel - inputLevel) > 15))    // medium change
           ||((now_time - last_update_time > 1200) && (abs(new_user_inputLevel - inputLevel) > 31))    // BIG change - every second
           ) && (now_time - lastInterfaceUpdate > INTERFACE_UPDATE_COOLDOWN))		                     // respect UI cooldown time
      {
        inputLevel = new_user_inputLevel;           // change of least 3 units -> update user variable
        updateInterfaces(CALL_MODE_WS_SEND);        // is this the correct way to notify UIs ? Yes says blazoncek
        last_update_time = now_time;
        last_user_inputLevel = new_user_inputLevel;
      }
    }
    lastMode = knownMode;

#if defined(MIC_LOGGER) || defined(MIC_SAMPLING_LOG) || defined(FFT_SAMPLING_LOG)
    EVERY_N_MILLIS(20) {
      logAudio();
    }
#endif

  }
  if (audioSyncEnabled & (1 << 0)) {    // Only run the transmit code IF we're in Transmit mode
    //Serial.println("Transmitting UDP Mic Packet");

      EVERY_N_MILLIS(20) {
        transmitAudioData();
      }

  }

  // Begin UDP Microphone Sync
  if (audioSyncEnabled & (1 << 1)) {    // Only run the audio listener code if we're in Receive mode
    if (millis()-lastTime > delayMs) {
      if (udpSyncConnected) {
        //Serial.println("Checking for UDP Microphone Packet");
        int packetSize = fftUdp.parsePacket();
        if (packetSize) {
          // Serial.println("Received UDP Sync Packet");
          uint8_t fftBuff[packetSize];
          fftUdp.read(fftBuff, packetSize);
          audioSyncPacket receivedPacket;
          memcpy(&receivedPacket, fftBuff, packetSize);
          for (int i = 0; i < 32; i++ ){
            myVals[i] = receivedPacket.myVals[i];
          }
          sampleAgc = receivedPacket.sampleAgc;
          rawSampleAgc = receivedPacket.sampleAgc;
          sample = receivedPacket.sample;
          sampleAvg = receivedPacket.sampleAvg;
          // VERIFY THAT THIS IS A COMPATIBLE PACKET
          char packetHeader[6];
          memcpy(&receivedPacket, packetHeader, 6);
          if (!(isValidUdpSyncVersion(packetHeader))) {
            memcpy(&receivedPacket, fftBuff, packetSize);
            for (int i = 0; i < 32; i++ ){
              myVals[i] = receivedPacket.myVals[i];
            }
            sampleAgc = receivedPacket.sampleAgc;
            rawSampleAgc = receivedPacket.sampleAgc;
            sample = receivedPacket.sample;
            sampleAvg = receivedPacket.sampleAvg;

            // Only change samplePeak IF it's currently false.
            // If it's true already, then the animation still needs to respond.
            if (!samplePeak) {
              samplePeak = receivedPacket.samplePeak;
            }
            //These values are only available on the ESP32
            for (int i = 0; i < 16; i++) {
              fftResult[i] = receivedPacket.fftResult[i];
            }

            FFT_Magnitude = receivedPacket.FFT_Magnitude;
            FFT_MajorPeak = receivedPacket.FFT_MajorPeak;
            //Serial.println("Finished parsing UDP Sync Packet");
          }
        }
      }
    }
  }
} // userLoop()
