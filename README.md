# Erg Fridge Display Setup

## Setting Up the Backend to Pull from Concept2 API
1. For the easiest configuration, clone this repo to a folder named 'erg-fridge-display-backend' in your Docker project folder.
2. Rename .env.example to .env. Follow the instructions in .env to obtain and set your Concept2 API key and set up your local networking config.
3. Compose up the project.
4. While in the project root folder, run the following to fetch all of your historical Concept2 logbook data and create the local database:
   `docker exec -it erg-fridge-display-backend python3 /app/fetch_c2.py`

### How the local database logic works:
The script will work from oldest to newest in the workout history to build a PR history. Note that PRs are only triggered for the following workouts:

- Timed PR for exact distance matches for the following distances: 1k, 2k, 5k, 10k meters
- Distance PR for exact time matches for the following timed workouts: 30 minutes, 1 hour

Additionally, the script will count the number of workouts exactly matching 42,195 meters (Concept2's exact definition of a marathon) and count the number of marathons the user has completed.

To override the exact match logic and create a new PR for a workout that doesn't match the exact filters, comment parsing is performed.
For example, you rowed for 45 minutes, but 43:42 into the workout you hit 10,000 meters, a new PR.
When submitting the workout to your Concept2 Logbook, write in the comments field something like "new 10k PR 43:42".
The script will ingest your workout total meters and time, but parse the comment and append the new PR to the 10k_PR.csv file.

The exact verbiage for a comment PR is not important.
As long as you include the distance or time string (1k, 2k, 5k, 10k, 30min, 1hr) and a valid colon-separated clock time (MM:SS or HH:MM:SS), you can write whatever else you want.
The time strings are fuzzy so things like "30min", "30 min", "30 mins", "30 minutes", "1hr" "1 hr" "1 hour" will all trigger to attempt to process a new PR.

## Serve the stats on your local network as a systemd service
1. If your server doesn't have it, install the `webhook` package.
2. systemd requires absolute paths. Run `which webhook` and note the path. Debian returns `/usr/bin/webhook`.
3. Rename erg-backend-hook.example.json to erg-backend-hook.json, and edit the file.
4. Modify the "execute-command" line to point to the `which webhook` path. Modify the "command-working-directory" line to point to your project root folder.
5. Open erg-backend-hook.example.service. Modify the file according to the comments. Copy the entire contents.
6. Create a new service config file as root, and paste in the contents of erg-backend-hook.example.service:
   `sudo nano /etc/systemd/system/erg-backend-hook.service`
7. Reload the systemd manager configuration to recognize the new service, enable it to run on boot, and start it immediately:
   ```
   sudo systemctl daemon-reload
   sudo systemctl enable erg-backend-hook.service
   sudo systemctl start erg-backend-hook.service
   ```
8. Check the status and the logs at any time:
   ```
   sudo systemctl status erg-backend-hook.service
   journalctl -u erg-backend-hook.service -f
   ```

## Upload the sketch to your ESP32

Assembly instructions for the hardware are on Printables:

1. Plug in the ESP32 via USB-C to your computer.
2. Discover ESP32 hardware address if it's your only serial device: `ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null`. Mine is at /dev/ttyACM0.
3. On Fedora and some other distros, your local user account cannot talk directly to hardware raw data tty nodes unless you are part of the hardware control group.
   Run the following user modification command so your account can execute raw writes over that port without needing root privileges:
   `sudo usermod -a -G dialout $USER`
   To apply, log out and back in, or run in the active terminal window `newgrp dialout`
4. If you don't have arduino-cli, download and install it to your home bin directory:
   `curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/bin sh`
   Ensure it's working with `arduino-cli version`
5. If you have multiple serial devices, can run `arduino-cli board list` now to find hardware address. 
6. Create a fresh config file: `arduino-cli config init`
7. Append the official ESP32 board manager package URL:
   `arduino-cli config set board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json`
8. Update the index files: `arduino-cli core update-index`
9. Install the core platform files for ESP32 (this replaces the graphical Board Manager): `arduino-cli core install esp32:esp32`
10. In the esp32 folder, compile the sketch targetting the generic ESP32 platform:
   `arduino-cli compile --fqbn esp32:esp32:esp32 .`
11. If you get a compilation error for missing libraries, e.g. fatal error: # GxEPD2_BW.h: No such file or directory, you need to install them:
   `arduino-cli lib install "GxEPD2" "ArduinoJson" "Adafruit GFX Library"`
12. Upload the binary code over the physical serial cable path (replace /dev/tty**** path with your output from earlier step)
   `arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32 .`
13. Unplug from USB-C.
14. Test the button click logic, a single click should update stats and print to the display. Double click will enter maintenance mode for 2 mins to allow updating the sketch.
15. After double click, test that your ESP32 has network access: `ping -c 4 erg-fridge-display.local`
16. Once everything is working, assemble your hardware. 

### Update OTA Later

Uploading over Wi-Fi, I was having trouble resolving the .local hostname in combination with an upload. So I switched to using the IP discovered in step 15 above.

For any changes to the .ino, just compile again and send with below. Hit enter if it asks for a password:

`arduino-cli upload -p 192.168.1.X -l network --discovery-timeout 15s --fqbn esp32:esp32:esp32 .`

### Changing ports

By default, this stack is set to run on port 8080 (Python script webhook) and port 8081 (rowing_stats.json Nginx server).

If you're already running services on these ports, you will have to change them in:

- ./docker-compose.yml
- erg-backend-hook.service (if you haven't already copied contents to `/etc/systemd/system/erg-backend-hook.service`
- ./esp32/ErgDisplayBaseline.ino under the // --- Server Configuration --- section
