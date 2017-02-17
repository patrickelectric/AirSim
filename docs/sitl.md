# Why Flight Controller
"Wait!" you ask, "Why do you need *flight controller* for simulator?". Flight controller has two essential elements (among *many* others) called pose estimation and high speed real time controller loop that generates motor signals for the drone. We consume these signals directly in our physics engine so the simulation is as close to real thing as possible. This is known as "hardware-in-loop" OR HIL simulation. HIL simulation is preferred for testing 
new flight algorithms in the simulator because they have a better chance of working on the real drone when we are done testing.

If you do not want to spend money on this extra hardware then you can probably get away with "software-in-loop" simulation (SITL). 
In this case, the firmware runs in your computer as opposed to flight controller. One downside to SITL mode is that your PC usually has more power than 
the tiny flight controller on your drone, so you may not get the same flying performance when trying the same thing on a real drone.
But it's still useful when you don't have a flight controller handy. It is just a few more steps to set up as shown below.

# Software-In-Loop Simulation (SITL)
1. Install [BashOnWindows](https://msdn.microsoft.com/en-us/commandline/wsl/install_guide).  You can also do this on an Ubuntu machine, but in that case
you will have to use real ip addresses instead of localhost.
2. Type `bash` at Windows command prompt. Follow [these steps for Linux](http://dev.px4.io/starting-installing-linux.html) and follow the instructions
under `NuttX based hardware` to install prerequisites.
3. Get the PX4 source code and build the SITL:
```
git clone https://github.com/PX4/Firmware.git
cd Firmware
git submodule update --init --recursive
make posix_sitl_default
```
3. Use following command to start PX4 firmware:
```
./build_posix_sitl_default/src/firmware/posix/px4 ./posix-configs/SITL/init/lpe/iris
```
4. Now you can run Unreal environment and connect to SITL via UDP[settings file](settings.md) with IP address 127.0.0.1 and port 14560.
If you are running a separate Ubuntu machine find its IP address and use that in the settings.json file.
5. You should also be able to use QGroundControl just like with actual [flight controller harware](prereq.md). 
Note that as we don't have physical board, RC cannot be connected directly to it. 
So the alternatives are either use XBox 360 Controller or connect your RC using USB port if it has it 
(for example, in case of [FrSky Taranis X9D Plus](prereq.md)) or using trainer USB cable to PC. 
This makes your RC look like joystick. You will need to do extra set up in QGroundControl to use RC control as below.

# Using Joystick/Gamepad (Alternative to RC)
Why do you need RC for simulator? Because usual joysticks are not very accurate and in fact very "noisy" for flying! 
We have tried it and gladly switched back to regular RC. But just in case you want to try it as well, here are the instructions.

1. Connect Joystick/Gamepad such as XBox 360 controller to PC. Start QGroundControl.
2. Click on the purple "Q" icon on left in tool bar to reveal the Preferences panel.
3. Go to General tab and check the Virtual Joystick checkbox.
4. Go back to settings screen (gears icon), click on Parameters tab, type `COM_RC_IN_MODE` in search box and change its value to either `Joystick/No RC Checks` or `Virtual RC by Joystick`.
5. You should now see a new tab "Joystick" where you can do the calibration.
