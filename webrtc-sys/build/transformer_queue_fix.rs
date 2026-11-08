// Copyright 2026 LiveKit, Inc.
// SPDX-License-Identifier: Apache-2.0

//! Backport the sender queue drain without modifying a shared prebuilt cache.

use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

/// Compile the pinned replacement with the bridge's target configuration and
/// replace exactly one member in a private copy of the WebRTC static archive.
pub(super) fn prepare(builder: &cc::Build, original_dir: &Path) -> PathBuf {
    let out =
        PathBuf::from(env::var_os("OUT_DIR").expect("Cargo OUT_DIR")).join("webrtc-queue-fix");
    fs::create_dir_all(&out).expect("create patched WebRTC output directory");
    let source = Path::new("vendor/webrtc/rtp_sender_video_frame_transformer_delegate.cc");
    println!("cargo:rerun-if-changed={}", source.display());
    let compiler = builder.get_compiler();
    let msvc = compiler.is_like_msvc();
    let object = out.join(if msvc { "sender_queue_fix.obj" } else { "sender_queue_fix.o" });
    let mut compile = compiler.to_command();
    if msvc {
        compile.arg("/c").arg(source).arg(format!("/Fo{}", object.display()));
    } else {
        compile.arg("-c").arg(source).arg("-o").arg(&object);
    }
    run(&mut compile, "compile WebRTC sender queue fix");

    let library_name = if msvc { "webrtc.lib" } else { "libwebrtc.a" };
    let original = original_dir.join(library_name);
    let patched = out.join(library_name);
    let mut list = builder.get_archiver();
    list.arg(if msvc { "/list" } else { "t" }).arg(&original);
    let output = list.output().expect("list WebRTC archive members");
    assert!(
        output.status.success(),
        "WebRTC archive listing failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let members = String::from_utf8(output.stdout).expect("archive member names are UTF-8");
    let matches: Vec<_> = members
        .lines()
        .map(str::trim)
        .filter(|member| {
            let name = member.rsplit(['/', '\\']).next().unwrap_or(member);
            matches!(
                name,
                "rtp_sender_video_frame_transformer_delegate.o"
                    | "rtp_sender_video_frame_transformer_delegate.obj"
            )
        })
        .collect();
    assert_eq!(
        matches.len(),
        1,
        "expected one sender transformer object in the pinned WebRTC archive"
    );
    let member = matches[0];
    if msvc {
        let stripped = out.join("without_sender.lib");
        run(
            builder
                .get_archiver()
                .arg("/nologo")
                .arg(format!("/OUT:{}", stripped.display()))
                .arg(format!("/REMOVE:{member}"))
                .arg(&original),
            "remove old sender transformer",
        );
        run(
            builder
                .get_archiver()
                .arg("/nologo")
                .arg(format!("/OUT:{}", patched.display()))
                .arg(&stripped)
                .arg(&object),
            "archive patched sender transformer",
        );
    } else {
        fs::copy(&original, &patched).expect("copy original WebRTC archive");
        run(
            builder.get_archiver().arg("d").arg(&patched).arg(member),
            "remove old sender transformer",
        );
        run(
            builder.get_archiver().arg("rs").arg(&patched).arg(&object),
            "archive patched sender transformer",
        );
    }
    out
}

/// Fail the build rather than silently linking the original crash-prone object.
fn run(command: &mut Command, operation: &str) {
    let status = command.status().unwrap_or_else(|error| panic!("{operation}: {error}"));
    assert!(status.success(), "{operation}: {status}");
}
