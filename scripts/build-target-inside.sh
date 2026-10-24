#!/bin/sh
set -eu
cd /src
mkdir -p build/bb10
cmake -S /src -B /src/build/bb10 -DCMAKE_TOOLCHAIN_FILE=/opt/bb10-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DPASSPORT_BB10=ON
cmake --build /src/build/bb10 --target bb10_tls test_bb10_tls test_standalone_consumer bb10_tls_probe test_target_coexistence todoist_network test_network todoist_ui test_ui todoist-passport -j2
/opt/bbndk/host/linux/x86/usr/bin/blackberry-nativepackager -devMode \
    -package /src/build/bb10/todoist-passport.bar \
    /src/bar-descriptor.xml -configuration Device-Debug
