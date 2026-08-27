# QLIC Rust decoder

Pure safe Rust. No dependencies, C, FFI, or unsafe code. Rust 1.85 or newer.

## Add it

```toml
[dependencies]
qlic-decoder = { path = "../qlic/Release 1.0/Source/rust/qlic-decoder" }
```

## Decode RGBA8

```rust
use qlic_decoder::{Limits, decode_rgba};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let encoded = std::fs::read("image.qlic")?;
    let image = decode_rgba(&encoded, &Limits::default())?;
    println!("{}x{}", image.width, image.height);
    use_rgba(&image.rgba);
    Ok(())
}

fn use_rgba(_: &[u8]) {}
```

The output is tightly packed RGBA8. Use `decode_rgba_into` when the caller
already owns a buffer. Use `decode_animation` for QAN1 or QAN2 animation.

## Decode wide or HDR data

```rust
use qlic_decoder::{Limits, WideSamples, decode_wide};

let encoded = std::fs::read("wide.qlic")?;
let image = decode_wide(&encoded, &Limits::default())?;
match image.samples {
    WideSamples::U16(samples) => use_u16(&samples),
    WideSamples::U32(samples) => use_u32(&samples),
}
# fn use_u16(_: &[u16]) {}
# fn use_u32(_: &[u32]) {}
# Ok::<(), Box<dyn std::error::Error>>(())
```

Use `decode_hdr` for integer samples plus ICC, CICP, mastering-display,
content-light, and alpha metadata. `HdrImage::metadata` retains every ordered
ancillary block as its four-byte tag and borrowed payload, including
EXIF/XMP/IPTC/JUMBF. Samples keep their declared precision. The decoder does no
tone mapping, color conversion, or record interpretation. Retained Rec. 2100
PQ and HLG fixtures verify transfer-characteristic values 16 and 18 plus exact
integer samples.

## Inspect or validate

`parse_info` reads the checked container and dimensions without entropy decode.
`validate` fully decodes and checks the file without keeping the output.

```rust
use qlic_decoder::{Limits, parse_info, validate};

let limits = Limits {
    max_pixels: 16_000_000,
    max_decoded_bytes: 256 * 1024 * 1024,
    max_metadata_bytes: 4 * 1024 * 1024,
    ..Limits::default()
};
let info = parse_info(&encoded, &limits)?;
validate(&encoded, &limits)?;
# Ok::<(), Box<dyn std::error::Error>>(())
```

Set lower limits for untrusted input. Allocations are fallible, checksums are
verified, and malformed data returns `Result` without panicking.

## Coverage

The decoder accepts every stream selected by the retained 3,167-image corpus,
plus QAN1/QAN2 animation, QSW1 wide samples, and QSW2 HDR fixtures. This is
current-encoder coverage, not a claim about every abandoned experimental mode.
Unsupported syntax returns a typed error.

Decode a file to PAM with:

```text
cargo run --release --example decode -- input.qlic output.pam
```

Run the crate checks with:

```text
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test --release
```

Apache-2.0. The clean-room LZMS port is also MIT; see `LICENSE-LZMS`.
