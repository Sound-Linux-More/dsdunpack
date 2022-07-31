`ORG.SLM:`
![GitHub release (latest by date)](https://img.shields.io/github/v/release/Sound-Linux-More/dsdunpack)
![GitHub Release Date](https://img.shields.io/github/release-date/Sound-Linux-More/dsdunpack)
![GitHub repo size](https://img.shields.io/github/repo-size/Sound-Linux-More/dsdunpack)
![GitHub all releases](https://img.shields.io/github/downloads/Sound-Linux-More/dsdunpack/total)
![GitHub](https://img.shields.io/github/license/Sound-Linux-More/dsdunpack)  

# DSD Unpack

This utility converts DSD audio between Sony DSF (.dsf) and Philips DSDIFF
(.dff) container formats, including decompression of DST-encoded DSDIFF files
and ID3 tags in both containers.

Parts of this project are based on the SACD Ripper project, particularly
libsacd and libdstdec. See https://sacd-ripper.github.io/ for more info.

Tested on Windows using MinGW 4.9.3 and MSVC 2013/2015. You will need
pthreads-win32 to build using MSVC.
