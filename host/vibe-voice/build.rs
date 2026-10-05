// Embed Info.plist into the executable so a plain (unbundled) binary can use
// privacy-protected APIs such as Speech Recognition without crashing.
fn main() {
    println!("cargo:rerun-if-changed=macos/Info.plist");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("macos") {
        let plist = std::path::Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap())
            .join("macos/Info.plist");
        println!(
            "cargo:rustc-link-arg-bins=-Wl,-sectcreate,__TEXT,__info_plist,{}",
            plist.display()
        );
    }
}
