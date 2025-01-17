#include "AmsMqttHandler.h"
#include "FirmwareVersion.h"
#include "AmsStorage.h"
#include "LittleFS.h"

void AmsMqttHandler::setCaVerification(bool caVerification) {
	this->caVerification = caVerification;
}

bool AmsMqttHandler::connect() {
	if(millis() - lastMqttRetry < 10000) {
		yield();
		return false;
	}
	lastMqttRetry = millis();

	time_t epoch = time(nullptr);

	if(mqttConfig.ssl) {
		if(epoch < FirmwareVersion::BuildEpoch) {
			if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("NTP not ready for MQTT SSL\n"));
			return false;
		}
		if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("MQTT SSL is configured (%dkb free heap)\n"), ESP.getFreeHeap());
		if(mqttSecureClient == NULL) {
			mqttSecureClient = new WiFiClientSecure();
			#if defined(ESP8266)
				mqttSecureClient->setBufferSizes(512, 512);
				if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf_P(PSTR("ESP8266 firmware does not have enough memory...\n"));
				return false;
			#endif
		
			if(caVerification && LittleFS.begin()) {
				File file;

				if(LittleFS.exists(FILE_MQTT_CA)) {
					if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Found MQTT CA file (%dkb free heap)\n"), ESP.getFreeHeap());
					file = LittleFS.open(FILE_MQTT_CA, (char*) "r");
					#if defined(ESP8266)
						BearSSL::X509List *serverTrustedCA = new BearSSL::X509List(file);
						mqttSecureClient->setTrustAnchors(serverTrustedCA);
					#elif defined(ESP32)
						if(mqttSecureClient->loadCACert(file, file.size())) {
							if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("CA accepted\n"));
						} else {
							if(debugger->isActive(RemoteDebug::WARNING)) debugger->printf_P(PSTR("CA was rejected\n"));
							delete mqttSecureClient;
							mqttSecureClient = NULL;
							return false;
						}
					#endif
					file.close();

					if(LittleFS.exists(FILE_MQTT_CERT) && LittleFS.exists(FILE_MQTT_KEY)) {
						#if defined(ESP8266)
							if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Found MQTT certificate file (%dkb free heap)\n"), ESP.getFreeHeap());
							file = LittleFS.open(FILE_MQTT_CERT, (char*) "r");
							BearSSL::X509List *serverCertList = new BearSSL::X509List(file);
							file.close();

							if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Found MQTT key file (%dkb free heap)\n"), ESP.getFreeHeap());
							file = LittleFS.open(FILE_MQTT_KEY, (char*) "r");
							BearSSL::PrivateKey *serverPrivKey = new BearSSL::PrivateKey(file);
							file.close();

							if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf_P(PSTR("Setting client certificates (%dkb free heap)"), ESP.getFreeHeap());
							mqttSecureClient->setClientRSACert(serverCertList, serverPrivKey);
						#elif defined(ESP32)
							if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Found MQTT certificate file (%dkb free heap)\n"), ESP.getFreeHeap());
							file = LittleFS.open(FILE_MQTT_CERT, (char*) "r");
							mqttSecureClient->loadCertificate(file, file.size());
							file.close();

							if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Found MQTT key file (%dkb free heap)\n"), ESP.getFreeHeap());
							file = LittleFS.open(FILE_MQTT_KEY, (char*) "r");
							mqttSecureClient->loadPrivateKey(file, file.size());
							file.close();
						#endif
					}
				} else {
					if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("No CA, disabling validation\n"));
					mqttSecureClient->setInsecure();
				}
				LittleFS.end();
			} else {
				if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("CA verification disabled\n"));
				mqttSecureClient->setInsecure();
			}
			mqttClient = mqttSecureClient;

			if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf_P(PSTR("MQTT SSL setup complete (%dkb free heap)\n"), ESP.getFreeHeap());
		}
	}
	
	if(mqttClient == NULL) {
		if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("No SSL, using client without SSL support\n"));
		mqttClient = new WiFiClient();
	}

    if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Connecting to MQTT %s:%d\n"), mqttConfig.host, mqttConfig.port);
	
	mqtt.begin(mqttConfig.host, mqttConfig.port, *mqttClient);

	#if defined(ESP8266)
		if(mqttSecureClient) {
			time_t epoch = time(nullptr);
			if(debugger->isActive(RemoteDebug::DEBUG)) debugger->printf_P(PSTR("Setting NTP time %lu for secure MQTT connection\n"), epoch);
			mqttSecureClient->setX509Time(epoch);
		}
	#endif

	// Connect to a unsecure or secure MQTT server
	if ((strlen(mqttConfig.username) == 0 && mqtt.connect(mqttConfig.clientId)) ||
		(strlen(mqttConfig.username) > 0 && mqtt.connect(mqttConfig.clientId, mqttConfig.username, mqttConfig.password))) {
		if(debugger->isActive(RemoteDebug::INFO)) debugger->printf_P(PSTR("Successfully connected to MQTT!\n"));
        return true;
	} else {
		if (debugger->isActive(RemoteDebug::ERROR)) {
			debugger->printf_P(PSTR("Failed to connect to MQTT: %d\n"), mqtt.lastError());
			#if defined(ESP8266)
				if(mqttSecureClient) {
					mqttSecureClient->getLastSSLError((char*) json, BufferSize);
					debugger->println((char*) json);
				}
			#endif
		}
        return false;
	}
}

void AmsMqttHandler::disconnect() {
    mqtt.disconnect();
    mqtt.loop();
    delay(10);
    yield();

	if(mqttClient != NULL) {
		mqttClient->stop();
		delete mqttClient;
		mqttClient = NULL;
		if(mqttSecureClient != NULL) {
			mqttSecureClient = NULL;
		}
	}
}

lwmqtt_err_t AmsMqttHandler::lastError() {
    return mqtt.lastError();
}

bool AmsMqttHandler::connected() {
	return mqtt.connected();
}

bool AmsMqttHandler::loop() {
    bool ret = mqtt.loop();
    delay(10);
    yield();
	#if defined(ESP32)
		esp_task_wdt_reset();
	#elif defined(ESP8266)
		ESP.wdtFeed();
	#endif
    return ret;
}