#!/usr/bin/env python3
"""THIRD_PARTY_NOTICES.md (the repository's root): every license of what the Real Engine is built with or ships, made
from the license files themselves so it never drifts. Run after updating a library in external/ or a font in data/fonts.
make_dist.ps1 copies it next to the editor; Build > Package Game copies it next to every game.
    python tools/make_notices.py
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PARTS = [
    ("GLFW 3.4", "Windows, input and the Vulkan surface (the editor and the games).", "zlib/libpng License", "external/glfw/LICENSE.md"),
    ("Dear ImGui", "The editor's interface and the games' debug views.", "MIT License", "external/cimgui/imgui/LICENSE.txt"),
    ("cimgui", "The C bindings of Dear ImGui.", "MIT License", "external/cimgui/LICENSE"),
    ("meshoptimizer", "Mesh optimization, LODs and the clusters of the ray tracing.", "MIT License", "external/meshoptimizer/LICENSE.md"),
    ("Inter (font)", "The interface's text.", "SIL Open Font License 1.1", "data/fonts/OFL-Inter.txt"),
    ("Sora (font)", "The titles.", "SIL Open Font License 1.1", "data/fonts/OFL-Sora.txt"),
    ("JetBrains Mono (font)", "The code editor.", "SIL Open Font License 1.1", "data/fonts/OFL-JetBrainsMono.txt"),
]


def main():
    out = []
    out.append("# Third-Party Notices\n")
    out.append("The Real Engine itself is under the MIT License (see `LICENSE`). It is built with, and ships, the works below; ")
    out.append("each keeps its own license, reproduced here as its authors require. Games made with the Real Engine carry this ")
    out.append("file too (Build > Package Game puts it next to the game).\n\n")
    out.append("The Real Engine is not affiliated with or endorsed by Epic Games, Inc. Unreal Engine and other product names ")
    out.append("mentioned in the documentation are trademarks of their owners and are used only to describe compatibility or ")
    out.append("to compare features.\n\n")
    out.append("## Summary\n\n| Component | Used for | License |\n|---|---|---|\n")
    for name, use, lic, _ in PARTS:
        out.append("| %s | %s | %s |\n" % (name, use, lic))
    out.append("| Demo assets (Poly Haven, ambientCG) | The demo levels (downloaded at build time into DemoContent/) | CC0 1.0 (public domain); the list in `DemoContent/LICENSES.txt` |\n")
    out.append("| NVIDIA DLSS, AMD FidelityFX FSR 3.1, Intel XeSS (optional) | Upscaling | Their vendors' licenses, shipped next to each DLL (`nvngx_dlss_LICENSE.txt`, `amd_fidelityfx_LICENSE.txt`, `libxess_LICENSE.txt`); not part of the source repository (`tools/fetch_upscalers.ps1`) |\n")
    out.append("| Steamworks API (optional) | Steam features of a game | Valve's Steamworks SDK license: a game that turns Steam on ships its developer's own `steam_api64.dll`; nothing of the SDK is in this repository |\n")
    out.append("| Cycles shading kernel (Blender Foundation) | The shader nodes' procedural textures follow its definitions | Apache License 2.0 |\n")
    out.append("| OpenXR API (Khronos) | The VR calls' declarations | Apache License 2.0 |\n")
    out.append("| Microsoft Visual C++ Runtime | The programs' C runtime | Microsoft's redistributable license (Visual Studio) |\n\n")
    for name, _, lic, path in PARTS:
        text = open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read().strip()
        out.append("## %s — %s\n\n```\n%s\n```\n\n" % (name, lic, text))
    # (2026-11-23) work derived from Apache-2.0 sources: the shading nodes' procedural textures follow Cycles' kernel (so a
    # node setup looks the same as in Blender), and the OpenXR calls are declared from Khronos' API
    out.append("## Cycles (Blender Foundation) and OpenXR (Khronos) — Apache License 2.0\n\n")
    out.append("The procedural textures and color math of the shader nodes (`shaders/tl/kernel_svm_tex.h`, `kernel_svm_sim.h`, ")
    out.append("`kernel_svm_types.h`) follow the definitions of Cycles' shading kernel (`svm_noise`, `svm_voronoi`, `svm_checker`, ")
    out.append("`svm_gradient`, `svm_wave`, `svm_math`, `svm_color_util`, `svm_brick`, `svm_magic`), Copyright Blender Foundation, ")
    out.append("licensed under the Apache License 2.0; they were rewritten for this engine (C11 / GLSL) so the same node setups look ")
    out.append("the same as in Blender. `src/xr_openxr.h` declares the part of the OpenXR 1.0 API (Copyright The Khronos Group, ")
    out.append("Apache License 2.0) the engine calls. The license:\n\n")
    out.append("```\n%s\n```\n\n" % open(os.path.join(ROOT, "data", "licenses", "Apache-2.0.txt"), encoding="utf-8").read().strip())
    demo = os.path.join(ROOT, "data", "demo", "LICENSES.txt")
    if os.path.exists(demo):
        out.append("## Demo assets — CC0 1.0\n\n```\n%s\n```\n" % open(demo, encoding="utf-8", errors="replace").read().strip())
    open(os.path.join(ROOT, "THIRD_PARTY_NOTICES.md"), "w", encoding="utf-8", newline="\n").write("".join(out))
    print("THIRD_PARTY_NOTICES.md: %d components" % (len(PARTS) + 4))


if __name__ == "__main__":
    main()
