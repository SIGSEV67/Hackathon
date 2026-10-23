# 13 · Referencias

## Plataforma (Espressif)

- ESP-IDF, *Programming Guide* para ESP32-S3: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/>
- ESP-NOW: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_now.html>
- WiFi (modos AP/STA/APSTA, FTM, ahorro de energía): <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi.html>
- I2S (estándar y PDM): <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html>
- GPIO e interrupciones: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html>
- `esp_lcd` (SSD1306): <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/lcd/index.html>
- Servidor HTTP: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/protocols/esp_http_server.html>
- Bus de eventos (`esp_event`): <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/esp_event.html>
- Sistema de build y componentes (`REQUIRES`, `PRIV_REQUIRES`, `EMBED_FILES`): <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/build-system.html>
- Kconfig / menuconfig: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/kconfig.html>
- NVS: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/nvs_flash.html>
- FreeRTOS (tasks, colas, semáforos): <https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/00-Developer-docs>

## Redes

- S.-Y. Ni, Y.-C. Tseng, Y.-S. Chen, J.-P. Sheu, *The broadcast storm problem in
  a mobile ad hoc network*, ACM MobiCom 1999. — Por qué el flooding ingenuo
  satura y cómo mitigarlo (jitter, supresión).
- A. Vahdat, D. Becker, *Epidemic Routing for Partially-Connected Ad Hoc
  Networks*, Duke University, Tech. Report CS-200006, 2000. — La base del
  store-and-forward del courier.
- K. Fall, *A Delay-Tolerant Network Architecture for Challenged Internets*,
  ACM SIGCOMM 2003. — Redes tolerantes a demoras (DTN).
- P. Levis, N. Patel, D. Culler, S. Shenker, *Trickle: A Self-Regulating
  Algorithm for Code Propagation and Maintenance in Wireless Sensor Networks*,
  USENIX NSDI 2004. — Supresión por escuchar a otros, como la del courier.
- S. Floyd, V. Jacobson, C.-G. Liu, S. McCanne, L. Zhang, *A Reliable Multicast
  Framework for Light-weight Sessions and Application Level Framing*, IEEE/ACM
  Transactions on Networking, 1997 (SRM). — Recuperación por NACK, como `repair.c`.
- A. Demers et al., *Epidemic Algorithms for Replicated Database Maintenance*,
  ACM PODC 1987. — Difusión epidémica (gossip).
- T. Clausen, P. Jacquet (eds.), *Optimized Link State Routing Protocol (OLSR)*,
  RFC 3626, 2003. — Reenvío por MPR, una mejora posible al flooding.
- Meshtastic, *Mesh Broadcast Algorithm*: <https://meshtastic.org/docs/overview/mesh-algo/> — Flooding administrado sobre LoRa.
- LoRa Alliance, *What is LoRaWAN?* y especificación regional: <https://lora-alliance.org/> — Tasas y *duty cycle* de LoRa.

## Radio y distancia

- IEEE Std 802.11-2016, *Wireless LAN Medium Access Control (MAC) and Physical
  Layer (PHY) Specifications* (incluye Fine Timing Measurement, de 802.11mc).
- T. S. Rappaport, *Wireless Communications: Principles and Practice*, 2.ª ed.,
  Prentice Hall, 2002, cap. 4. — Modelo de pérdida log-distancia.
- T. M. J. Fruchterman, E. M. Reingold, *Graph Drawing by Force-directed
  Placement*, Software: Practice and Experience 21(11), 1991. — El layout del mapa.

## Audio

- IMA Digital Audio Focus and Technical Working Groups, *Recommended Practices
  for Enhancing Digital Audio Compatibility in Multimedia Systems*, rev. 3.00, 1992. — Define IMA ADPCM.
- Analog Devices, *MAX98357A/MAX98357B datasheet*: <https://www.analog.com/en/products/max98357a.html>
- Codec2 (comparación, códec de voz para radio): <https://github.com/drowe67/codec2>
- Opus, RFC 6716 (comparación).

## Web

- RFC 8910, *Captive-Portal Identification in DHCP and Router Advertisements*, 2020.
- RFC 8908, *Captive Portal API*, 2020.
- RFC 8952, *Captive Portal Architecture*, 2020.
- RFC 1035, *Domain Names — Implementation and Specification*, 1987.
- MDN, *Secure contexts*: <https://developer.mozilla.org/en-US/docs/Web/Security/Secure_Contexts>
- MDN, `BaseAudioContext.decodeAudioData`: <https://developer.mozilla.org/en-US/docs/Web/API/BaseAudioContext/decodeAudioData>
- MDN, atributo HTML `capture`: <https://developer.mozilla.org/en-US/docs/Web/HTML/Attributes/capture>
- MDN, *Autoplay guide*: <https://developer.mozilla.org/en-US/docs/Web/Media/Autoplay_guide>
- MDN, `Crypto.getRandomValues`: <https://developer.mozilla.org/en-US/docs/Web/API/Crypto/getRandomValues>
- Fuente Silkscreen (Jason Kottke, SIL Open Font License): <https://fonts.google.com/specimen/Silkscreen>
- Estilo de la hackatón Ghost in the HAL: <https://comcom.dc.uba.ar/hackaton/>

## Seguridad (para el trabajo futuro)

- D. J. Bernstein, *Curve25519: new Diffie-Hellman speed records*, PKC 2006.
- TweetNaCl.js: <https://tweetnacl.js.org/>
