# NVIDIA Streamline headers

The public headers of the NVIDIA Streamline SDK 2.14.16 (github.com/NVIDIA-RTX/Streamline, release
v2.14.16, `include/`), unchanged, under the MIT license in `LICENSE.txt`. Only the headers the port
uses are here; `sl_nvperf.h` (Nsight Perf SDK license) and the helpers are left out.

Only `gpu/shim/gow3_streamline.cpp` includes them. It loads `sl.interposer.dll` at run time; the
port links nothing from Streamline. The Streamline DLLs come from the same release
(`bin/x64`, NVIDIA-signed production builds).
