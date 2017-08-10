HELLO example


## Table of Contents

  * [Prerequisites](#prerequisites)
    * [About environment variables](#about-environment-variables)
    * [About adding locations to the PATH variable](#about-adding-locations-to-the-path-variable)
    * [Qt](#qt)
    * [Visual Studio 12\.0 (Community is enough)](#visual-studio-120-community-is-enough)
    * [Android Studio](#android-studio)
    * [Android SDK and tools versions](#android-sdk-and-tools-versions)
    * [NDK](#ndk)
    * [CMake 3\.3\.2](#cmake-332)
    * [ant 1\.9\.4](#ant-194)
    * [Java 1\.8](#java-18)
    * [GNU Make for Windows](#gnu-make-for-windows)
    * [Python](#python)
    * [Check prerequisites](#check-prerequisites)
  * [Environment](#environment)
    * [Create a standalone toolchain (android NDK)](#create-a-standalone-toolchain-android-ndk)
    * [Important About Android Build Tools The "android" command is deprecated\.](#important-about-android-build-tools-the-android-command-is-deprecated)
    * [CMake](#cmake)
    * [Scribe](#scribe)
    * [cp\.bat](#cpbat)
  * [Libraries](#libraries)
    * [Google Gvr sdk](#google-gvr-sdk)
    * [OpenSSL](#openssl)
  * [HiFi](#hifi)
    * [Fix toolchain file in HiFi](#fix-toolchain-file-in-hifi)
  * [Environment variables recap](#environment-variables-recap)
  * [Build](#build)
    * [Create a build directory](#create-a-build-directory)
    * [CMake](#cmake-1)
    * [make](#make)
    * [Command failed "make" error message](#command-failed-make-error-message)
    * [Currently known problem with '\.\.' command\.](#currently-known-problem-with--command)
  * [Appendix I (Troubleshooting) Could not find Qt5LinguistTools](#appendix-i-troubleshooting-could-not-find-qt5linguisttools)
  * [Appendix II (Troubleshooting) Android device](#appendix-ii-troubleshooting-android-device)
    * [Enable USB Debugging](#enable-usb-debugging)
    * [Huawei Mate 9 Pro logcat](#huawei-mate-9-pro-logcat)
    * [Daydream setup](#daydream-setup)
  * [Appendix III (Troubleshooting) Weird errors (must see)](#appendix-iii-troubleshooting-weird-errors-must-see)

Created by [gh-md-toc](https://github.com/ekalinin/github-markdown-toc.go)

## Prerequisites

### About environment variables

The build process requires environment variables to be set for some software and libraries.
For example, a QT_CMAKE_PREFIX_PATH for a Qt path and ANDROID_NDK for the NDK path should be set. To set the former, on the terminal we should run:

````
set QT_CMAKE_PREFIX_PATH=C:\Qt\Qt5.6.1\5.6\android_armv7\lib\cmake
setx QT_CMAKE_PREFIX_PATH=%QT_CMAKE_PREFIX_PATH%
````

Notice that setx will make persistent the variables for new terminal windows (but not for the already opened ones). You can also add the environment variable [manually] (https://www.microsoft.com/resources/documentation/windows/xp/all/proddocs/en-us/sysdm_advancd_environmnt_addchange_variable.mspx?mfr=true)

### About adding locations to the PATH variable

Some steps described here require to add tools locations in the PATH variable. [How-to by Microsoft](https://msdn.microsoft.com/en-us/library/office/ee537574(v=office.14).aspx).

### Qt
Tested in [Qt 5.6.1-1](http://download.qt.io/official_releases/qt/5.6/5.6.1-1/qt-opensource-windows-x86-android-5.6.1-1.exe.mirrorlist).
Newer versions like Qt 5.6.2 may have problems with the build process.

Environment variable QT_CMAKE_PREFIX_PATH should target the `C:\Qt\Qt5.6.1\5.6\android_armv7\lib\cmake`

For example if Qt was installed in C:\Qt\Qt5.6.1 :
````
"C:\Qt\Qt5.6.1\5.6\android_armv7\lib\cmake"
````

### Visual Studio 12.0 (Community is enough)

Visual Studio 12 2013 is needed. Newer versions have not been tested.

### Android Studio
https://developer.android.com/studio/index.html

### Android SDK and tools versions
Inside the package manager (with android studio installed)
- Android SDK Api level 24
- sdk tools 25.2.2
- sdk platform-tools 25

If you are installing a recent version of android sdk check this [important note](#important-about-android-build-tools-the-android-command-is-deprecated)

### NDK
If the ndk-bundle is a different version than ndk r12b, download it separately: 
[Android NDK r12b](https://developer.android.com/ndk/downloads/older_releases.html#ndk-12b-downloads)

ndk-bundle or the uncompressed folder for r12b should be in the environment variable ANDROID_NDK **using unix slashes**.

````
set ANDROID_NDK=C:/Users/user/workspace-hifi/android-ndk-r12b
setx ANDROID_NDK=%ANDROID_NDK%
````

### CMake 3.3.2
The build process was fully tested and done with [CMake 3.3.2](https://cmake.org/files/v3.3/cmake-3.3.2-win32-x86.zip). Newer versions also worked but may have deprecated some macros and functions that would require changes in our CMake files.

### ant 1.9.4

Download [ant](http://ant.apache.org/bindownload.cgi), install it and add the **bin** directory to the PATH variable

### Java 1.8

Java is needed on the final step to package the apk. Be sure [Java 8](http://www.oracle.com/technetwork/java/javase/downloads/jdk8-downloads-2133151.html) is installed in your system and remember to set JAVA_HOME pointing to your JDK.

````
C:\Users\user>javac -version
javac 1.8.0_131
C:\Users\user>

````

````
set JAVA_HOME=C:\Program Files\Java\jdk1.8.0_131
setx JAVA_HOME %JAVA_HOME%
````

### GNU Make for Windows 

GNU Make is needed to build the all the libraries and the project itself. Download [gnuwin32](http://gnuwin32.sourceforge.net/packages/make.htm).

### Python

Python is used to execute the Android NDK script that creates a standalone toolchain. Download and install [Python 2.7.12](https://www.python.org/ftp/python/2.7.12/python-2.7.12.msi). (Newer versions haven't been tested but may work).

### Check prerequisites

With the utility [checkhifi.bat](https://drive.google.com/open?id=0BzVk5wZGx4ZtaFZudjluQzNXcU0) you can check if you have the needed variables and programs.

## Environment

### Create a standalone toolchain (android NDK)
The toolchain for HiFi must target API Level 24, for ARM and using GNU STL. The build process needs a toolchain called "my-tc-24-gnu" in the toolchaing folder inside the ndk. It can be generated running:
`%ANDROID_NDK%\build\tools\make_standalone_toolchain.py --arch arm --api 24 --stl=gnustl --install-dir %ANDROID_NDK%\toolchains\my-tc-24-gnu`

### Important About Android Build Tools The "android" command is deprecated.

Newer android tools will not run `android update lib-project` which was deprecated:

````
**************************************************************************
The "android" command is deprecated.
For manual SDK, AVD, and project management, please use Android Studio.
For command-line tools, use tools\bin\sdkmanager.bat
and tools\bin\avdmanager.bat
**************************************************************************

Invalid or unsupported command ""

Supported commands are:
android list target
android list avd
android list device
android create avd
android move avd
android delete avd
android list sdk
android update sdk
[...]
````

Our current toolchain still uses the non gradle build setup for Android, so to make it work a downgrade is needed:
1. Download [Tools r25.2.2](https://dl.google.com/android/repository/tools_r25.2.2-windows.zip)
2. Backup your tools dir inside your Android SDK home
3. Copy the uncompressed tools directory from tools_r25.2.2-windows.zip into the Android SDK home, replacing the previous/original one.
For example:
With ANDROID_HOME as C:/Users/user/AppData/Local/Android/sdk
````
...
 |---- Android
        |------- sdk
                  |------- toolsbackup  (the original one, new, that deprecates what we need)
                  |------- tools        (tools_r25.2.2-windows downloaded)
````

### CMake

We use CMake to generate the makefiles that compile and deploy the Android APKs to your device. In order to create Makefiles for the Android targets, CMake requires that some environment variables are set, and that other variables are passed to it when it is run.

The following must be set in your environment:

* ANDROID_NDK - the root of your Android NDK install
* ANDROID_HOME - the root of your Android SDK install
* ANDROID_LIB_DIR - the directory containing cross-compiled versions of dependencies. (Details below)

##### About ANDROID_LIB_DIR

In general 'hifi' is cloned inside ANDROID_LIB_DIR and other dependencies end up being sister directories of 'hifi'.
For example:
````
workspace-hifi  (ANDROID_LIB_DIR)
|-- hifi (Clone from https://github.com/highfidelity/hifi.git )
|-- gvr-android-sdk
|-- openssl
|-- etc..
````

### Scribe
High Fidelity has a shader pre-processing tool called scribe that various libraries will call on during the build process.
CMake will have a fatal error if it does not find the scribe executable while using the android toolchain.

#### Precompiled binary (recommended)
[Download](https://drive.google.com/open?id=0BzVk5wZGx4Ztc0tpelgxdjF1dkU) and uncompress it in any folder. That folder path should be set in an ENV variable SCRIBE_PATH.

#### Build it yourself (skip if you have a binary)
Follow the [instructions](./BUILD_SCRIBE_WIN.md) to build scribe and then set an ENV variable SCRIBE_PATH that is a path where the scribe executable is.

#### Scribe path

For example, if the scribe executable is in a tools directory:

````
workspace-hifi  (ANDROID_LIB_DIR)
|-- hifi (Clone from https://github.com/highfidelity/hifi.git )
|-- gvr-android-sdk
|-- openssl
|-- tools
	 |------ scribe (executable)
````
SCRIBE_PATH should be `C:\Users\user\workspace-hifi\tools`

### cp.bat

CMake generates makefiles that use `cp` (Unix name) instead of `copy` (which is the command-line tool to copy files on Windows).

A workaround is done by placing a [cp.bat script](https://drive.google.com/open?id=0BzVk5wZGx4ZtYVBhaDgyM0UzcTg) in the PATH.

(That script simply calls `copy`)

Another untested approach is installing GNU tools on Windows through [Cygwin](https://www.cygwin.com/) or [MiniGW](http://www.mingw.org/). The `cp.bat` approach is still more lightweight.

## Libraries

### Google Gvr sdk
Clone this project https://github.com/googlevr/gvr-android-sdk.git into the workspace folder (folder that will contain hifi repo too, so if "hifi" is the checkouted folder, gvr-android-sdk should be a sister of it).
Current integration targets GVR SDK version 1.40.0, so cloning it should retrieve all tags, then run:
 `$ git checkout tags/v1.40.0 -b branch_1_40_0`
 (Or any name you want for that branch)
Finally, set the ENV variable HIFI_ANDROID_GVR as the name of this cloned repository:
For example:
Example, having:
````
workspace-hifi  (ANDROID_LIB_DIR)
|-- hifi (Clone from `https://github.com/highfidelity/hifi.git` )
|-- gvr-android-sdk
|-- etc..
````
HIFI_ANDROID_GVR must be 'gvr-android-sdk' (full path is completed with ANDROID_LIB_DIR)

### OpenSSL

#### Precompiled binary (recommended)
[Download](https://www.dropbox.com/s/0ozqzfh9rh0mdzs/openssl.tar.gz?dl=0) and unpack it on the same workspace folder, being sister of hifi and gvr-android-sdk).

It's important to preserve permissions and symbolic links in that archive.

WinRAR successfully uncompresses keeping file permissions and symbolic links.

Another way to uncompress it, is running from a Git Bash Terminal (or any bash terminal on Windows):

````
tar -zxvf openssl.tar.gz
````

#### Build it yourself  (skip if you have a binary)

Check [Build OpenSSL in Mac](/BUILD_ANDROID_MAC.md#build-it-yourself-skip-if-you-have-a-binary-1).

## HiFi

Clone `https://github.com/highfidelity/hifi.git` (conveniently inside ANDROID_LIB_DIR) and then switch to a branch with the latest Android source code.

Branch "Android" should be the one to use. If you check that this file BUILD_ANDROID_MAC.md is not in the root dir, then switch because necessary changes to make the android app run will not be there.

### Fix toolchain file in HiFi

File `cmake\android\android.toolchain.cmake` needs a change to support the clang.cmd executable from the ndk on Windows.

Download this already [modified version](https://drive.google.com/open?id=0B7PJO9XsQw2gNWxmRzJIbmFyc3c) and replace your file.

An alternative is to apply [this patch](https://gist.github.com/Cristo86/d1fc1439cb145ea50528717408df3399) but using the already modified version is still recommended.


## Environment variables recap

Check all your environment variables and check the Notes below:

````
C:\> SET ANDROID_HOME="C:/Users/user/AppData/Local/Android/sdk"
C:\> SET ANDROID_LIB_DIR="C:/Users/user/workspace-hifi"
C:\> SET ANDROID_NDK="C:/Users/user/workspace-hifi/android-ndk-r12b"
C:\> SET HIFI_ANDROID_GVR="gvr-android-sdk-upd"
C:\> SET SCRIBE_PATH="C:\Users\user\workspace-hifi\tools"
C:\> SET QT_CMAKE_PREFIX_PATH=C:\Qt\Qt5.6.1\5.6\android_armv7\lib\cmake
C:\> SET PATH=%PATH%:"C:\Program Files\ant\bin:%JAVA_HOME%\bin"

C:\> SETX ANDROID_HOME %ANDROID_HOME%
C:\> SETX ANDROID_LIB_DIR %ANDROID_LIB_DIR%
C:\> SETX ANDROID_NDK %ANDROID_NDK%
C:\> SETX HIFI_ANDROID_GVR %HIFI_ANDROID_GVR%
C:\> SETX SCRIBE_PATH %SCRIBE_PATH%
C:\> SETX QT_CMAKE_PREFIX_PATH %QT_CMAKE_PREFIX_PATH%
C:\> SETX PATH %PATH%

````
#### Notes
* ANDROID_HOME targets the sdk, which in this case was installed with Android Studio. If using an standalone SDK, set that path as that variable.
* PATH is just an example, as there may be more paths in other systems (ant is important to be there).

## Build

### Create a build directory

Create a folder "build" (recommended) inside the hifi folder. Actually any place is ok but some instructions here suppose that location.

### CMake

The following must be passed to CMake when it is run:

* USE_ANDROID_TOOLCHAIN - set to true to build for Android

Example running CMake inside a build dir (which itself is inside the project dir e.g. 'hifi')

````
cmake -G "Unix Makefiles" -DUSE_NSIGHT=0 -DUSE_ANDROID_TOOLCHAIN=1 -DANDROID_QT_CMAKE_PREFIX_PATH=%QT_CMAKE_PREFIX_PATH% ..
````
Where QT_CMAKE_PREFIX_PATH must be `C:\QtInstallDir\5.6\android_armv7\lib\cmake`
(With QtInstallDir is the correct path where Qt for Android was installed)

### make

After cmake: run to build the apk (inside the build folder).

````
make interface-apk
````

Following described problems are very likely to happen:

### Command failed "make" error message

A possible not so descriptive error may occur: 

````
CMake Error at C:/Users/user/dev/workspace-hifi/hifi/build-android-00/ext/android/makefiles/bullet/project/src/bullet-stamp/bullet-build-.cmake:16 (message):
  Command failed: 2

   'make'

  See also

    C:/Users/user/dev/workspace-hifi/hifi/build-android-00/ext/android/makefiles/bullet/project/src/bullet-stamp/bullet-build-*.log


make[3]: *** [ext/android/makefiles/bullet/project/src/bullet-stamp/bullet-build] Error 1
make[2]: *** [ext/android/makefiles/bullet/CMakeFiles/bullet.dir/all] Error 2
````

If checking the bullet-build-*.log the last successful output (in a file `bullet-build-out.log` in this case) was like

````
Linking CXX shared library..
````

Maybe the command to execute the linker is [too long](https://support.microsoft.com/en-us/help/830473/command-prompt-cmd.-exe-command-line-string-limitation) for the Windows command line to handle.

A workaround is done by setting the following 4 flags in ext\android\makefiles\bullet\project\src\bullet\CMakeLists.txt and retrying `make interface-apk`

````
cmake_minimum_required
# workaround: add these 4 lines 
SET(CMAKE_C_USE_RESPONSE_FILE_FOR_OBJECTS 1)
SET(CMAKE_CXX_USE_RESPONSE_FILE_FOR_OBJECTS 1)
SET(CMAKE_C_RESPONSE_FILE_LINK_FLAG "@")
SET(CMAKE_CXX_RESPONSE_FILE_LINK_FLAG "@")
...
````



### Currently known problem with '..' command.

During the build process, when scribe is needed to execute, an error about unknown '..' commands will happen. Following instructions make it possible to continue building:

#### Fix makefiles that use scribe
Generated makefiles use .. which is not working on Windows. So we must replace
  ```
  ../../../tools/scribe/build/Debug/scribe.exe
  ```
  for
  ```
  c:/Users/user/dev/workspace-hifi/hifi/tools/scribe/build/Debug/scribe.exe
  ```
  (Or in your case your full path to scribe)

In several files, e.g. `<build-dir>\libraries\display-plugins\CMakeFiles\display-plugins.dir\build.make`

To automate that process, use the `fixscribeinmakes.bat` script described below:

#### Use fixscribeinmakes.bat

First of all, `fixscribeinmakes.bat` requires the script [replacerx.bat](https://drive.google.com/open?id=0BzVk5wZGx4Zta1JIYzk1Mjh6Q0E) to be in the PATH so it can be run by simply typing `replacerx`.

[Download fixscribeinmakes.bat](https://drive.google.com/open?id=0BzVk5wZGx4ZteHdOM3lSZnpXelk) and edit it, particularly to match paths in your system. Must check these variables inside:

`FIX_SCRIBE_OLD` - Path to scribe.exe that starts with ../../.. which is not supported and needs to be replaced

`FIX_SCRIBE_NEW` - Full path to the scribe.exe tool

`FIX_HIFI_LIBRARIES_DIR` - Full path should point your current <build-dir>\libraries

As we use full paths fixscribeinmakes.bat can be run from anywhere (anyway, it's recommended to place it in your workspace-folder, outside 'hifi').

Note: The replace string routine may take some time so, wait until the entire script is done.

After running `fixscribeinmakes.bat` run inside the build directory, `make interface-apk` again.

## Appendix I (Troubleshooting) Could not find Qt5LinguistTools

If an error like to following happens:

````
Could not find a package configuration file provided by "Qt5LinguistTools"
 with any of the following names:

   Qt5LinguistToolsConfig.cmake
   qt5linguisttools-config.cmake

 Add the installation prefix of "Qt5LinguistTools" to CMAKE_PREFIX_PATH or
 set "Qt5LinguistTools_DIR" to a directory containing one of the above
 files.  If "Qt5LinguistTools" provides a separate development package or
 SDK, be sure it has been installed.
````

Check these requirements:
1. Env variable QT_CMAKE_PREFIX_PATH should target the android_armv7/lib/cmake as a full path like `C:\Qt\Qt5.6.1\5.6\android_armv7\lib\cmake`
2. Qt for android should be [version 5.6.1](http://download.qt.io/official_releases/qt/5.6/5.6.1/qt-opensource-mac-x64-android-5.6.1.dmg.mirrorlist).

## Appendix II (Troubleshooting) Android device

Some setup that comes in handy when dealing with Android devices.

### Enable USB Debugging

(Instructions and menu flows for other devices may vary)

Settings -> about phone -> click "build number" several times until a "You are now a Developer" message pops up.

Then in "Developer options" menu, enable "Developer options" and "USB Debugging".

### Huawei Mate 9 Pro logcat

By default, logs are turned off on that phone. To enable them:

Dial

````
*#*#2846579#*#*
````
and you will see a hidden menu. Go to the Project Menu > Background Setting > Log setting and define the log availability (log switch) and level (log level setting). [source](https://stackoverflow.com/a/18395092/939781)

### Daydream setup

Full Daydream setup (includes entering payment information) is required to properly run Daydream and Daydream Apps like Hifi "Interface".


## Appendix III (Troubleshooting) Weird errors (must see)

#### Path errors

At least ANDROID_LIB_DIR and NDK_PATH should use / slashes
```
ANDROID_LIB_DIR
C:\Users\user\dev\workspace-hifi\hifi>set ANDROID_LIB_DIR=c:/Users/user/dev/workspace-hifi/
C:\Users\user\dev\workspace-hifi\hifi>setx ANDROID_LIB_DIR %ANDROID_LIB_DIR%
```

#### Some    l i b w h a t v e r . s o    (e.g. like it being searching for wrong lib names - missing e) not found

Whenever the linker does not find a library, just provide the one with the name "suggested".

Example of the situation with a missing 's' in libprocedural.so (so it looks for libprocedural.o):
```
  [ 86%] Linking CXX shared library apk/libs/armeabi-v7a/libinterface.so
  clang++.exe: error: no such file or directory: 'apk/libs/armeabi-v7a/libprocedural.o'
  make[3]: *** [interface/apk/libs/armeabi-v7a/libinterface.so] Error 1
  make[2]: *** [interface/CMakeFiles/interface.dir/all] Error 2
  make[1]: *** [interface/CMakeFiles/interface-apk.dir/rule] Error 2
  make: *** [interface-apk] Error 2
```
Create a copy of the existing libprocedural.so file as *libprocedural.o* and then run make interface-apk again

**It is pending to discover why that happens, this is just a workaround to make it possible to build**

#### Syntax error when linking libinterface.so

Edit the build_dir/interface/CMakeFiles/interface.dir/build.make:
Look for the string
```
Linking CXX shared library apk/libs/armeabi-v7a/libinterface.so
```
Next to it there should be a call to *clang++.cmd*, replace it for **clang38++.exe**

#### build.xml error when running qtcreateapk

Apparently the code that runs "android update" on the gvr-common library doesn't run at all on Windows.
Manually run inside build-dir/interface/gvr-common and build-dir/interface/gvr-base:
```
%ANDROID_HOME%/tools/android update lib-project -p . -t android-24
```
(our current target is android-24)
Run interface-apk again inside build-dir.




