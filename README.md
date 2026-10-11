This project is about making a lora based tracking system for felines (cats).

The project is located in a jungle area in Philippines, so we cannot use internet/mobile data based solutions.

Hence why we will be using a LORA based radio location system.

The system will be composed of 3 parts.

1. Cat collar tracking device. This device will be attached to the cats collar, and tracks it where abouts as it is roaming around. This collar needs to be lightweight and
   have good battery life. This will be utilizing a TENSTAR ROBOT NRF52840 Pro Micro and a RA-02 module. And a 150mA battery.
2. Handheld homing device. This device will be operated by a human to locate the cats by strength of radio signal. Will be using a esp32-c3 and a ra-02 module. Can use a 18650 battery or similar for power. Will also have a speaker and screen for visual/audio feedback.
3. Base stations. These will serve as a network of stations to receive pings from cat collar, to continually track the cats as they are roaming around. Perhaps using
   triagulation to pinpoint the cats exact location on a map.

For now, this project is all about getting part 1 and 2 up and running. Part 3 of base stations will come later as I will receive more electronic components for the project.
