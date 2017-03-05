#__pvr.hdhomerundvr__  

Unofficial Kodi HDHomeRun DVR PVR Client   
##[__DOCUMENTATION AND DOWNLOADS__](https://github.com/djp952/pvr.hdhomerundvr/wiki)   
   
Copyright (C)2017 Michael G. Brehm    
[MIT LICENSE](https://opensource.org/licenses/MIT)   
   
[__CURL__](https://curl.haxx.se/) - Copyright (C)1996 - 2017, Daniel Stenberg, daniel@haxx.se, and many contributors   
[__OPENSSL__](https://www.openssl.org/) - Copyright (C)1998-2016 The OpenSSL Project   
[__ZLIB__](http://www.zlib.net/) - Copyright (C)1995-2017 Jean-loup Gailly and Mark Adler   
   
**BUILD ENVIRONMENT**  
* Windows 10 x64 15025   
* Visual Studio 2013   
* Visual Studio 2015 (with Git for Windows)   
* Bash on Ubuntu on Windows 16.04.1 LTS   
* Android NDK r12b for Windows 64-bit
   
**CONFIGURE BASH ON UBUNTU ON WINDOWS**   
Open "Bash on Ubuntu on Windows"   
```
sudo apt-get update
sudo apt-get install gcc g++ gcc-multilib g++-multilib gcc-4.9 g++-4.9 gcc-4.9-multilib g++-4.9-multilib
```
   
**CONFIGURE ANDROID NDK**   
Download the Android NDK r12b for Windows 64-bit:    
[https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip](https://dl.google.com/android/repository/android-ndk-r12b-windows-x86_64.zip)   

* Extract the contents of the .zip file somewhere   
* Set a System Environment Variable named ANDROID_NDK_ROOT that points to the extraction location (android-ndk-r12b)   
* Optionally, ANDROID_NDK_ROOT can also be set on the command line prior to executing msbuild:   
```
...
set ANDROID_NDK_ROOT=D:\android-ndk-r12b
msbuild msbuild.proj
...
```
   
**BUILD**   
Open "Developer Command Prompt for VS2015"   
```
git clone https://github.com/djp952/build --depth=1
git clone https://github.com/djp952/external-kodi-addon-dev-kit -b Jarvis --depth=1
git clone https://github.com/djp952/external-sqlite -b sqlite-3.17.0 --depth=1
git clone https://github.com/djp952/prebuilt-libcurl -b libcurl-7.52.1 --depth=1
git clone https://github.com/djp952/prebuilt-libssl -b libssl-1.0.2k --depth=1
git clone https://github.com/djp952/prebuilt-libuuid -b libuuid-1.0.3 --depth=1
git clone https://github.com/djp952/prebuilt-libz -b libz-1.2.8 --depth=1
git clone https://github.com/djp952/pvr.hdhomerundvr -b Jarvis
cd pvr.hdhomerundvr
msbuild msbuild.proj

> out\zuki.pvr.hdhomerundvr-win32-jarvis-x.x.x.x.zip (Windows / Win32)
> out\zuki.pvr.hdhomerundvr-linux-i686-jarvis-x.x.x.x.zip (Linux / i686)
> out\zuki.pvr.hdhomerundvr-linux-x86_64-jarvis-x.x.x.x.zip (Linux / x86_64)
> out\zuki.pvr.hdhomerundvr-android-arm-jarvis-x.x.x.x.zip (android-arm)
> out\zuki.pvr.hdhomerundvr-android-x86-jarvis-x.x.x.x.zip (android-x86)
```
