use std::fs;
use std::path::{Path, PathBuf};

use qlic_decoder::{
    AlphaAssociation, ColorAuthority, Container, Error, LimitKind, Limits, Mode, crc32, parse_info,
    parse_qsw1,
};

fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn fixture(name: &str) -> Vec<u8> {
    fs::read(fixture_dir().join(name)).unwrap()
}

fn rewrite_crc(bytes: &mut [u8]) {
    let end = bytes.len() - 4;
    let checksum = crc32(&bytes[..end]).to_le_bytes();
    bytes[end..].copy_from_slice(&checksum);
}

#[test]
fn parses_every_committed_qlic_fixture() {
    let expected = [
        ("animation.qlic", 2, 2, Mode::Animation),
        ("blocks.qlic", 1, 17, Mode::Blocks),
        ("cpalette-lzms.qlic", 512, 512, Mode::CompressedPalette),
        ("gray-model-lzms.qlic", 384, 256, Mode::GrayModel),
        ("gray-rle.qlic", 3, 31, Mode::Gray),
        ("hdr-u16-10-hlg-rgb.qlic", 2, 2, Mode::HdrWide),
        ("hdr-u16-12-pq-rgba.qlic", 2, 2, Mode::HdrWide),
        ("described-u16-8-srgb-rgb.qlic", 2, 2, Mode::HdrWide),
        ("native.qlic", 64, 64, Mode::Native),
        ("normal-map-quadratic.qlic", 128, 256, Mode::Native),
        ("palette-filtered.qlic", 9, 257, Mode::PaletteStream),
        ("palette.qlic", 17, 1, Mode::Palette),
        ("planar-med-lzms.qlic", 3, 2, Mode::Rgb),
        ("rgb-lzms.qlic", 257, 9, Mode::Rgb),
        ("separable.qlic", 1, 1, Mode::Separable),
        ("tile-model.qlic", 31, 3, Mode::TileModel),
        ("tile-palette-lzms.qlic", 542, 699, Mode::CompressedPalette),
        ("wide-u16-10-boundary.qlic", 3, 2, Mode::NativeWide),
        ("wide-u16-16-rgba.qlic", 2, 2, Mode::NativeWide),
        ("wide-u32-17-boundary.qlic", 3, 2, Mode::NativeWide),
        ("wide-u32-24-rgb.qlic", 2, 2, Mode::NativeWide),
    ];

    let mut names = fs::read_dir(fixture_dir())
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "qlic")
        })
        .map(|path| path.file_name().unwrap().to_string_lossy().into_owned())
        .collect::<Vec<_>>();
    names.sort();
    assert!(names.len() >= expected.len());

    for (name, width, height, mode) in expected {
        assert!(names.iter().any(|candidate| candidate == name));
        let bytes = fixture(name);
        let container = Container::parse(&bytes, &Limits::default()).unwrap();
        assert_eq!(container.header.width, width, "{name}");
        assert_eq!(container.header.height, height, "{name}");
        assert_eq!(container.header.mode, mode, "{name}");
        assert_eq!(container.header.compressed_size, container.payload.len());
        assert_eq!(container.header.palette_size, container.palette.len());
        assert_eq!(
            parse_info(&bytes, &Limits::default()).unwrap(),
            container.info(&Limits::default()).unwrap()
        );
        if mode == Mode::NativeWide {
            let descriptor = parse_qsw1(&container).unwrap();
            assert_eq!(descriptor.bits_per_sample, container.header.index_bits);
            assert_eq!(
                u32::from(descriptor.channels),
                container.header.palette_count
            );
        }
        if mode == Mode::HdrWide {
            let info = parse_info(&bytes, &Limits::default()).unwrap();
            if matches!(
                name,
                "described-u16-8-srgb-rgb.qlic" | "hdr-u16-10-hlg-rgb.qlic"
            ) {
                assert_eq!(info.alpha_association, Some(AlphaAssociation::None));
                assert_eq!(info.color_authority, Some(ColorAuthority::CicpOnly));
                assert!(!info.has_icc && info.has_cicp);
                assert!(!info.has_mastering_display && !info.has_content_light);
            } else {
                assert_eq!(info.alpha_association, Some(AlphaAssociation::Straight));
                assert_eq!(info.color_authority, Some(ColorAuthority::IccPreferred));
                assert!(info.has_icc && info.has_cicp);
                assert!(info.has_mastering_display && info.has_content_light);
            }
        }
    }

    for name in names {
        let bytes = fixture(&name);
        Container::parse(&bytes, &Limits::default()).unwrap();
        parse_info(&bytes, &Limits::default()).unwrap();
    }
}

#[test]
fn reports_animation_and_still_info_like_the_c_api() {
    let animation = fixture("animation.qlic");
    let info = parse_info(&animation, &Limits::default()).unwrap();
    assert_eq!((info.frame_count, info.animated), (2, true));
    assert_eq!((info.channels, info.bits_per_sample), (4, 8));

    let still = fixture("native.qlic");
    let info = parse_info(&still, &Limits::default()).unwrap();
    assert_eq!((info.frame_count, info.animated), (1, false));
}

#[test]
fn rejects_truncation_and_container_corruption_for_every_fixture() {
    for entry in fs::read_dir(fixture_dir()).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().is_none_or(|extension| extension != "qlic") {
            continue;
        }
        let bytes = fs::read(&path).unwrap();
        for length in [0, 3, 27, bytes.len() - 1] {
            assert!(
                Container::parse(&bytes[..length], &Limits::default()).is_err(),
                "accepted {length}-byte truncation of {}",
                path.display()
            );
        }
        let mut corrupt = bytes;
        corrupt[28] ^= 0x40;
        assert!(matches!(
            Container::parse(&corrupt, &Limits::default()),
            Err(Error::ContainerChecksumMismatch { .. })
        ));
    }
}

#[test]
fn enforces_file_pixel_payload_and_animation_caps() {
    let native = fixture("native.qlic");
    let mut limits = Limits {
        max_file_bytes: native.len() as u64 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        Container::parse(&native, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::FileBytes,
            ..
        })
    ));

    limits = Limits::default();
    limits.max_pixels = 64 * 64 - 1;
    assert!(matches!(
        Container::parse(&native, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Pixels,
            ..
        })
    ));

    limits = Limits::default();
    limits.max_payload_bytes = 40;
    assert!(matches!(
        Container::parse(&native, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::PayloadBytes,
            ..
        })
    ));

    let animation = fixture("animation.qlic");
    limits = Limits::default();
    limits.max_frames = 1;
    assert!(matches!(
        Container::parse(&animation, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Frames,
            ..
        })
    ));
}

#[test]
fn rejects_invalid_mode_codec_and_palette_headers_with_valid_crc() {
    let original = fixture("palette.qlic");

    let mut invalid_mode = original.clone();
    invalid_mode[12] = 6;
    rewrite_crc(&mut invalid_mode);
    assert!(matches!(
        Container::parse(&invalid_mode, &Limits::default()),
        Err(Error::InvalidMode(6))
    ));

    let mut invalid_codec = original.clone();
    invalid_codec[15] = 0x81;
    rewrite_crc(&mut invalid_codec);
    assert!(matches!(
        Container::parse(&invalid_codec, &Limits::default()),
        Err(Error::InvalidCodec(0x81))
    ));

    let mut missing_palette = original;
    missing_palette[16..20].copy_from_slice(&0_u32.to_le_bytes());
    rewrite_crc(&mut missing_palette);
    assert!(matches!(
        Container::parse(&missing_palette, &Limits::default()),
        Err(Error::InvalidHeader("invalid palette header"))
    ));
}

fn qsw1_file(bits: u8, channels: u8, lengths: &[u64], blobs: &[&[u8]]) -> Vec<u8> {
    let mut payload = Vec::new();
    payload.extend_from_slice(b"QSW1");
    payload.extend_from_slice(&[0, bits, channels, bits.div_ceil(8)]);
    payload.extend_from_slice(&0x1234_5678_u32.to_le_bytes());
    payload.extend_from_slice(&0_u32.to_le_bytes());
    for &length in lengths {
        payload.extend_from_slice(&length.to_le_bytes());
    }
    for blob in blobs {
        payload.extend_from_slice(blob);
    }

    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&2_u32.to_le_bytes());
    file.extend_from_slice(&3_u32.to_le_bytes());
    file.extend_from_slice(&[19, 0, bits, 0x80]);
    file.extend_from_slice(&u32::from(channels).to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(&payload);
    let checksum = crc32(&file);
    file.extend_from_slice(&checksum.to_le_bytes());
    file
}

#[test]
fn parses_checked_qsw1_slice_ranges_without_decoding_pixels() {
    let file = qsw1_file(9, 3, &[2, 3], &[b"ab", b"cde"]);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    let descriptor = parse_qsw1(&container).unwrap();
    assert_eq!(descriptor.bits_per_sample, 9);
    assert_eq!(descriptor.channels, 3);
    assert_eq!(descriptor.sample_crc32, 0x1234_5678);
    assert_eq!(descriptor.sample_count, 18);
    assert_eq!(descriptor.decoded_bytes, 36);
    assert_eq!(descriptor.slices.len(), 2);
    assert_eq!(descriptor.slices[0].range, 32..34);
    assert_eq!(descriptor.slices[1].range, 34..37);
}

#[test]
fn rejects_qsw1_zero_overlong_and_trailing_slice_layouts() {
    let zero = qsw1_file(9, 1, &[0, 1], &[b"x"]);
    let container = Container::parse(&zero, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw1(&container),
        Err(Error::InvalidQsw1("zero-length byte slice"))
    ));

    let overlong = qsw1_file(9, 1, &[2, 1], &[b"x", b"y"]);
    let container = Container::parse(&overlong, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw1(&container),
        Err(Error::InvalidQsw1("byte slice exceeds payload"))
    ));

    let trailing = qsw1_file(9, 1, &[1, 1], &[b"x", b"y", b"z"]);
    let container = Container::parse(&trailing, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw1(&container),
        Err(Error::InvalidQsw1("trailing data after byte slices"))
    ));
}

#[test]
fn qsw1_requires_outer_size_match_and_obeys_decoded_cap() {
    let mut size_mismatch = qsw1_file(17, 4, &[1, 1, 1], &[b"a", b"b", b"c"]);
    size_mismatch[20..28].copy_from_slice(&1_u64.to_le_bytes());
    rewrite_crc(&mut size_mismatch);
    let container = Container::parse(&size_mismatch, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw1(&container),
        Err(Error::InvalidQsw1(
            "stored payload size does not match outer header"
        ))
    ));

    let file = qsw1_file(17, 4, &[1, 1, 1], &[b"a", b"b", b"c"]);
    let limits = Limits {
        max_decoded_bytes: 95,
        ..Limits::default()
    };
    assert!(matches!(
        Container::parse(&file, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 96,
            ..
        })
    ));
}
