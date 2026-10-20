#!/usr/bin/env bash
# The files for NVIDIA DLAA and AMD FSR 4 Native AA (in-game menu -> Image -> Anti-aliasing), which
# are not part of this repository:
#   - NVIDIA DLSS SDK v310.9.16 (github.com/NVIDIA/DLSS, NVIDIA's license: LICENSE.txt): headers,
#     nvsdk_ngx_s.lib and nvngx_dlss.dll into downloads/dlss-sdk-<version>; with MSVC (Visual
#     Studio 2022 or its Build Tools) builds gow3_pc/gpu/dlss_bridge -> gow3_dlss.dll and puts
#     both DLLs in gow3_pc/out beside the runtime. About 63 MB.
#   - FSR 4 v07 INT8 native model (AMD's MIT-licensed FidelityFX SDK source, built by Q2RTX:
#     github.com/FireBurn/Q2RTX) into gow3_pc/fsr4_shaders, the 1080 and 2160 tiers. About 6 MB.
# Usage: tools/fetch_aa_files.sh [dlss] [fsr4] (default both). Files already there are kept.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
want=${*:-dlss fsr4}

fetch() {  # fetch URL FILE [SHA256]: downloads once, through a .part file; a wrong hash deletes it
    # A kept file must match too; one that does not is dropped and fetched again.
    if [[ -s $2 ]]; then
        [[ -z ${3:-} || $(sha256sum -- "$2" | cut -d' ' -f1) == "$3" ]] && return 0
        echo "SHA-256 mismatch for kept $2: fetching again" >&2
        rm -f -- "$2"
    fi
    mkdir -p -- "$(dirname -- "$2")"
    curl -fsSL --retry 3 -o "$2.part" "$1"
    if [[ -n ${3:-} ]] && [[ $(sha256sum -- "$2.part" | cut -d' ' -f1) != "$3" ]]; then
        rm -f -- "$2.part"
        echo "SHA-256 mismatch for $2 (expected $3)" >&2
        exit 1
    fi
    mv -- "$2.part" "$2"
}

if [[ $want == *dlss* ]]; then
    version=310.9.16
    sdk=downloads/dlss-sdk-$version
    # 20 Oct 2026 audit: pinned to the commit tag v$version pointed to (a tag can be moved) and the
    # binaries checked by SHA-256, because nvsdk_ngx_s.lib is linked into the shipped gow3_dlss.dll.
    commit=9981c6350dc58c0ce21f024fd65e31e851c31689
    base=https://raw.githubusercontent.com/NVIDIA/DLSS/$commit
    declare -A sha=(
        [LICENSE.txt]=d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0
        [lib/Windows_x86_64/x64/nvsdk_ngx_s.lib]=7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765
        [lib/Windows_x86_64/rel/nvngx_dlss.dll]=d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e
        [include/nvsdk_ngx.h]=dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
        [include/nvsdk_ngx_defs.h]=ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
        [include/nvsdk_ngx_defs_dlssd.h]=2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716
        [include/nvsdk_ngx_defs_dlssg.h]=c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1
        [include/nvsdk_ngx_defs_vk.h]=0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031
        [include/nvsdk_ngx_helpers.h]=5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471
        [include/nvsdk_ngx_helpers_cuda.h]=f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042
        [include/nvsdk_ngx_helpers_d3d.h]=ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba
        [include/nvsdk_ngx_helpers_dlssd.h]=a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532
        [include/nvsdk_ngx_helpers_dlssd_cuda.h]=7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730
        [include/nvsdk_ngx_helpers_dlssd_d3d.h]=6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293
        [include/nvsdk_ngx_helpers_dlssd_vk.h]=11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b
        [include/nvsdk_ngx_helpers_dlssg.h]=6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f
        [include/nvsdk_ngx_helpers_dlssg_d3d.h]=6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220
        [include/nvsdk_ngx_helpers_dlssg_vk.h]=b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316
        [include/nvsdk_ngx_helpers_vk.h]=c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623
        [include/nvsdk_ngx_loader.h]=abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75
        [include/nvsdk_ngx_params.h]=943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
        [include/nvsdk_ngx_params_dlssd.h]=d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e
        [include/nvsdk_ngx_params_dlssg.h]=c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad
        [include/nvsdk_ngx_standalone_common.h]=38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53
        [include/nvsdk_ngx_standalone_cuda.h]=b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499
        [include/nvsdk_ngx_vk.h]=2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d
    )
    for file in "${!sha[@]}"; do
        fetch "$base/$file" "$sdk/$file" "${sha[$file]}"
    done
    # The bridge: MSVC through CMake's Visual Studio generator. Vulkan headers from the MSYS2
    # CLANG64 tree, copied alone (its C library headers must not reach MSVC).
    msys=${MSYS2_ROOT:-/c/msys64}
    msys=${msys%/}  # on GitHub's runner the MSYS2 root is "/": not "//clang64" (a network path)
    vulkan=downloads/vulkan-include
    mkdir -p "$vulkan"
    cp -r "$msys/clang64/include/vulkan" "$msys/clang64/include/vk_video" "$vulkan/"
    # Visual Studio (any edition with the C++ tools) through its own CMake; the generator follows
    # its version (17 = 2022, 18 = 2026: GitHub's windows-latest runners move between them).
    cmake_exe=$(command -v cmake.exe || true)
    generator="Visual Studio 17 2022"
    vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    if [[ -x $vswhere ]]; then
        vs=$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
             -property installationPath | tr -d '\r')
        major=$("$vswhere" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
                -property installationVersion | tr -d '\r' | cut -d. -f1)
        [[ $major == 18 ]] && generator="Visual Studio 18 2026"
        if [[ -n $vs && -x "$(cygpath -u "$vs")/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" ]]; then
            cmake_exe="$(cygpath -u "$vs")/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
        fi
    fi
    [[ -n $cmake_exe && -x $cmake_exe ]] || { echo "No MSVC (Visual Studio Build Tools) found: DLSS bridge not built" >&2; exit 1; }
    root=$(cygpath -w "$PWD")
    "$cmake_exe" -S "$root\\gow3_pc\\gpu\\dlss_bridge" -B "$root\\gow3_pc\\out\\dlss-bridge" \
        -G "$generator" -A x64 -DDLSS_SDK_ROOT="$root\\downloads\\dlss-sdk-$version" \
        -DVULKAN_INCLUDE="$root\\downloads\\vulkan-include" > /dev/null
    "$cmake_exe" --build "$root\\gow3_pc\\out\\dlss-bridge" --config Release > /dev/null
    cp gow3_pc/out/dlss-bridge/Release/gow3_dlss.dll "$sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" gow3_pc/out/
    echo "DLSS: gow3_pc/out/gow3_dlss.dll and nvngx_dlss.dll ($version)"
fi

if [[ $want == *fsr4* ]]; then
    commit=ae8d628fae208813172446d1e49ed94150b04658
    base="https://raw.githubusercontent.com/FireBurn/Q2RTX/$commit/baseq2/fsr4_shaders"
    dest=gow3_pc/fsr4_shaders
    model=fsr4_model_v07_i8_native
    files=(LICENSE-FSR4-v07.txt rcas.spv fsr4_rcas.spv spd_auto_exposure.spv "${model}_pre.spv" "${model}_post.spv")
    # Native AA, and the upscaling models a render resolution below the output picks by its
    # scale (1.5 Quality, 1.7 Balanced, 2 Performance, more Ultra Performance): ~5 MB each.
    for preset in native quality balanced performance ultraperf; do
        model=fsr4_model_v07_i8_$preset
        files+=("${model}_initializers.bin" "${model}_pre_weights.bin" "${model}_shader_manifest.json")
        for tier in 1080 2160; do
            files+=("${model}_${tier}_pre.spv" "${model}_${tier}_post.spv")
            for pass in $(seq 1 12); do
                files+=("${model}_${tier}_pass${pass}.spv")
            done
        done
    done
    for file in "${files[@]}"; do
        fetch "$base/$file" "$dest/$file"
    done
    echo "FSR 4: ${#files[@]} files in $dest"
fi
