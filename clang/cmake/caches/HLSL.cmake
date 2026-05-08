# Including the native target is important because some of LLVM's tests fail if
# you don't.
set(LLVM_TARGETS_TO_BUILD "Native;SPIRV" CACHE STRING "")

# Include the DirectX target for DXIL code generation.
set(LLVM_EXPERIMENTAL_TARGETS_TO_BUILD "DirectX" CACHE STRING "")

set(LLVM_ENABLE_PROJECTS "clang;clang-tools-extra" CACHE STRING "")

set(CLANG_ENABLE_HLSL On CACHE BOOL "")

if (HLSL_ENABLE_DISTRIBUTION)
  set(LLVM_DISTRIBUTION_COMPONENTS
      "clang;hlsl-resource-headers;clangd"
      CACHE STRING "")
endif()

# Enable the offload test suite distribution. This includes all tool binaries
# and test files needed to run the HLSL offload test suite on another machine.
#
# Install with:
#   cmake --build build --target install-distribution
#
# Prerequisites on the target machine:
#   - Python 3.6+
#   - pip install lit pyyaml
#   - GPU drivers (D3D12, Vulkan, or Metal depending on test suite)
#   - DXC compiler (for non-clang test suites; clang-dxc is included)
#
# After installing, configure and run tests:
#   cd <prefix>/share/hlsl-test-suite
#   ./configure-test-suite.py --suite clang-d3d12
#   lit run/test/clang-d3d12
if (HLSL_ENABLE_OFFLOAD_DISTRIBUTION)
  # Utilities (FileCheck, split-file, etc.) require LLVM_INSTALL_UTILS.
  set(LLVM_INSTALL_UTILS ON CACHE BOOL "")
  set(LLVM_DISTRIBUTION_COMPONENTS
      "clang;hlsl-resource-headers;offloader;api-query;imgdiff;FileCheck;split-file;obj2yaml;not;offload-test-suite"
      CACHE STRING "")
endif()
