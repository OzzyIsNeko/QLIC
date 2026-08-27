use std::fs;
use std::ops::Range;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{
    AlphaAssociation, ColorAuthority, Container, Error, LimitKind, Limits, WideSamples, crc32,
    decode_hdr, decode_wide, parse_qst1, parse_qsw1, parse_qsw2,
};

fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn fixture(name: &str) -> Vec<u8> {
    fs::read(fixture_dir().join(name)).unwrap()
}

fn rewrite_outer_crc(bytes: &mut [u8]) {
    let footer = bytes.len() - 4;
    let checksum = crc32(&bytes[..footer]).to_le_bytes();
    bytes[footer..].copy_from_slice(&checksum);
}

fn rewrite_qst_and_outer_crc(bytes: &mut [u8], range: Range<usize>) {
    let mut qst = bytes[range.clone()].to_vec();
    qst[26..30].fill(0);
    let checksum = crc32(&qst).to_le_bytes();
    bytes[range.start + 26..range.start + 30].copy_from_slice(&checksum);
    rewrite_outer_crc(bytes);
}

fn qst_ranges(file: &[u8], hdr: bool) -> Vec<Range<usize>> {
    let outer = Container::parse(file, &Limits::default()).unwrap();
    if hdr {
        let descriptor = parse_qsw2(&outer, &Limits::default()).unwrap();
        let base = descriptor.pixel_payload.as_ptr() as usize - file.as_ptr() as usize;
        descriptor
            .pixels
            .slices
            .iter()
            .map(|slice| base + slice.range.start..base + slice.range.end)
            .collect()
    } else {
        let descriptor = parse_qsw1(&outer).unwrap();
        let base = outer.payload.as_ptr() as usize - file.as_ptr() as usize;
        descriptor
            .slices
            .iter()
            .map(|slice| base + slice.range.start..base + slice.range.end)
            .collect()
    }
}

#[test]
fn retained_wide_slice_modes_and_transforms_are_explicit() {
    type SliceHeader = (u8, u8, u8);
    let wide_cases: [(&str, &[SliceHeader]); 4] = [
        ("wide-u16-10-boundary.qlic", &[(0, 0, 0), (0, 0, 0)]),
        ("wide-u16-16-rgba.qlic", &[(1, 0, 0), (1, 0, 0)]),
        (
            "wide-u32-17-boundary.qlic",
            &[(0, 0, 0), (0, 0, 4), (0, 0, 0)],
        ),
        (
            "wide-u32-24-rgb.qlic",
            &[(37, 0, 0), (37, 0, 0), (37, 0, 0)],
        ),
    ];
    for (name, expected) in wide_cases {
        let file = fixture(name);
        let outer = Container::parse(&file, &Limits::default()).unwrap();
        let wide = parse_qsw1(&outer).unwrap();
        let actual = wide
            .slices
            .iter()
            .map(|slice| {
                let qst =
                    parse_qst1(&outer.payload[slice.range.clone()], &Limits::default()).unwrap();
                (qst.info.mode, qst.info.transform, qst.info.flags)
            })
            .collect::<Vec<_>>();
        assert_eq!(actual, expected, "{name}");
    }

    let file = fixture("hdr-u16-12-pq-rgba.qlic");
    let outer = Container::parse(&file, &Limits::default()).unwrap();
    let hdr = parse_qsw2(&outer, &Limits::default()).unwrap();
    let actual = hdr
        .pixels
        .slices
        .iter()
        .map(|slice| {
            let qst =
                parse_qst1(&hdr.pixel_payload[slice.range.clone()], &Limits::default()).unwrap();
            (qst.info.mode, qst.info.transform, qst.info.flags)
        })
        .collect::<Vec<_>>();
    assert_eq!(actual, [(1, 0, 0), (37, 29, 0)]);
}

#[test]
fn decodes_all_retained_wide_fixtures_exactly() {
    let cases_u16: [(&str, u8, u8, usize, &[u16]); 2] = [
        (
            "wide-u16-10-boundary.qlic",
            1,
            10,
            6,
            &[0, 1, 511, 512, 1022, 1023],
        ),
        (
            "wide-u16-16-rgba.qlic",
            4,
            16,
            16,
            &[
                0, 1, 65_534, 65_535, 0x1234, 0x5678, 0x9abc, 0xdef0, 65_535, 0, 32_768, 1, 42,
                4_242, 60_000, 32_767,
            ],
        ),
    ];
    for (name, channels, bits, stride, expected) in cases_u16 {
        let image = decode_wide(&fixture(name), &Limits::default()).unwrap();
        assert_eq!(
            (image.width, image.height),
            (if channels == 1 { 3 } else { 2 }, 2)
        );
        assert_eq!((image.channels, image.bits_per_sample), (channels, bits));
        assert_eq!(image.stride, stride);
        assert_eq!(image.samples.as_u16(), Some(expected));
        assert_eq!(image.samples.as_u32(), None);
    }

    let cases_u32: [(&str, u8, u8, usize, &[u32]); 2] = [
        (
            "wide-u32-17-boundary.qlic",
            1,
            17,
            12,
            &[0, 1, 65_535, 65_536, 131_070, 131_071],
        ),
        (
            "wide-u32-24-rgb.qlic",
            3,
            24,
            24,
            &[
                0, 1, 0xff_ffff, 0x12_3456, 0xab_cdef, 0x80_0000, 0xff_fffe, 0x01_0203, 0xfe_dcba,
                0x00_ff00, 0xff_0000, 0x00_00ff,
            ],
        ),
    ];
    for (name, channels, bits, stride, expected) in cases_u32 {
        let image = decode_wide(&fixture(name), &Limits::default()).unwrap();
        assert_eq!(
            (image.width, image.height),
            (if channels == 1 { 3 } else { 2 }, 2)
        );
        assert_eq!((image.channels, image.bits_per_sample), (channels, bits));
        assert_eq!(image.stride, stride);
        assert_eq!(image.samples.as_u32(), Some(expected));
        assert_eq!(image.samples.as_u16(), None);
    }
}

#[test]
fn decodes_retained_hdr_pixels_and_borrows_icc_metadata() {
    let file = fixture("hdr-u16-12-pq-rgba.qlic");
    let image = decode_hdr(&file, &Limits::default()).unwrap();
    assert_eq!((image.pixels.width, image.pixels.height), (2, 2));
    assert_eq!(
        (image.pixels.channels, image.pixels.bits_per_sample),
        (4, 12)
    );
    assert_eq!(image.pixels.stride, 16);
    assert_eq!(
        image.pixels.samples.as_u16(),
        Some(
            [
                0, 1, 4_095, 4_095, 4_095, 2_048, 1_024, 3_000, 17, 255, 256, 1, 0x123, 0x456,
                0x789, 0xabc,
            ]
            .as_slice()
        )
    );
    assert_eq!(image.alpha_association, AlphaAssociation::Straight);
    assert_eq!(image.color_authority, ColorAuthority::IccPreferred);
    let expected_icc = b"\0\0\0\x10acspQLIC\x20\x26\x08\x14";
    let icc = image.icc.unwrap();
    assert_eq!(icc, expected_icc);
    let file_range = file.as_ptr_range();
    assert!(icc.as_ptr() >= file_range.start && icc.as_ptr() < file_range.end);
    assert_eq!(image.cicp.unwrap().color_primaries, 9);
    assert_eq!(image.cicp.unwrap().transfer_characteristics, 16);
    assert_eq!(image.cicp.unwrap().matrix_coefficients, 0);
    assert!(image.cicp.unwrap().full_range);
    assert_eq!(
        image.mastering_display.unwrap().primary_x,
        [35_400, 8_500, 6_550]
    );
    assert_eq!(
        image.mastering_display.unwrap().primary_y,
        [14_600, 39_850, 2_300]
    );
    assert_eq!(image.mastering_display.unwrap().white_x, 15_635);
    assert_eq!(image.mastering_display.unwrap().white_y, 16_450);
    assert_eq!(image.mastering_display.unwrap().max_luminance, 10_000_000);
    assert_eq!(image.mastering_display.unwrap().min_luminance, 50);
    assert_eq!(image.content_light.unwrap().max_cll, 1_000);
    assert_eq!(image.content_light.unwrap().max_fall, 400);
}

#[test]
fn decodes_retained_hlg_samples_and_cicp_exactly() {
    let file = fixture("hdr-u16-10-hlg-rgb.qlic");
    let image = decode_hdr(&file, &Limits::default()).unwrap();
    assert_eq!((image.pixels.width, image.pixels.height), (2, 2));
    assert_eq!(
        (image.pixels.channels, image.pixels.bits_per_sample),
        (3, 10)
    );
    assert_eq!(
        image.pixels.samples.as_u16(),
        Some([0, 1, 1023, 1023, 512, 256, 17, 511, 1000, 64, 900, 333].as_slice())
    );
    assert_eq!(image.alpha_association, AlphaAssociation::None);
    assert_eq!(image.color_authority, ColorAuthority::CicpOnly);
    assert!(image.icc.is_none());
    let cicp = image.cicp.unwrap();
    assert_eq!(
        (
            cicp.color_primaries,
            cicp.transfer_characteristics,
            cicp.matrix_coefficients,
            cicp.full_range
        ),
        (9, 18, 0, true)
    );
    assert!(image.mastering_display.is_none());
    assert!(image.content_light.is_none());
}

#[test]
fn decodes_self_describing_eight_bit_samples_exactly() {
    let file = fixture("described-u16-8-srgb-rgb.qlic");
    let image = decode_hdr(&file, &Limits::default()).unwrap();
    assert_eq!((image.pixels.width, image.pixels.height), (2, 2));
    assert_eq!(
        (image.pixels.channels, image.pixels.bits_per_sample),
        (3, 8)
    );
    assert_eq!(image.pixels.stride, 12);
    assert_eq!(
        image.pixels.samples.as_u16(),
        Some([0, 1, 255, 17, 127, 254, 255, 128, 2, 33, 66, 99].as_slice())
    );
    assert_eq!(image.alpha_association, AlphaAssociation::None);
    assert_eq!(image.color_authority, ColorAuthority::CicpOnly);
    assert!(image.icc.is_none());
    let cicp = image.cicp.unwrap();
    assert_eq!(
        (
            cicp.color_primaries,
            cicp.transfer_characteristics,
            cicp.matrix_coefficients,
            cicp.full_range
        ),
        (1, 13, 0, true)
    );
    assert!(image.mastering_display.is_none());
    assert!(image.content_light.is_none());
}

#[test]
fn wide_decode_enforces_limits_and_sample_checksum() {
    let original = fixture("wide-u16-10-boundary.qlic");
    let limits = Limits {
        max_decoded_bytes: 11,
        ..Limits::default()
    };
    assert!(matches!(
        decode_wide(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::DecodedBytes,
            actual: 12,
            ..
        })
    ));

    let mut corrupt_checksum = original;
    corrupt_checksum[36] ^= 1;
    rewrite_outer_crc(&mut corrupt_checksum);
    assert!(decode_wide(&corrupt_checksum, &Limits::default()).is_err());
}

#[test]
fn public_wide_and_hdr_decode_never_panic_on_prefixes_or_rechecksummed_mutations() {
    let fixtures = [
        ("wide-u16-10-boundary.qlic", false),
        ("wide-u16-16-rgba.qlic", false),
        ("wide-u32-17-boundary.qlic", false),
        ("wide-u32-24-rgb.qlic", false),
        ("described-u16-8-srgb-rgb.qlic", true),
        ("hdr-u16-10-hlg-rgb.qlic", true),
        ("hdr-u16-12-pq-rgba.qlic", true),
    ];
    let limits = Limits {
        max_file_bytes: 1 << 20,
        max_payload_bytes: 1 << 20,
        max_pixels: 1 << 18,
        max_decoded_bytes: 1 << 20,
        ..Limits::default()
    };
    for (name, hdr) in fixtures {
        let original = fixture(name);
        for length in 0..original.len() {
            let result = catch_unwind(AssertUnwindSafe(|| {
                if hdr {
                    let _ = decode_hdr(&original[..length], &limits);
                } else {
                    let _ = decode_wide(&original[..length], &limits);
                }
            }));
            assert!(result.is_ok(), "panicked on {name} prefix {length}");
        }
        for offset in 28..original.len() - 4 {
            let mut mutated = original.clone();
            mutated[offset] ^= 0xa5;
            rewrite_outer_crc(&mut mutated);
            let result = catch_unwind(AssertUnwindSafe(|| {
                if hdr {
                    let _ = decode_hdr(&mutated, &limits);
                } else {
                    let _ = decode_wide(&mutated, &limits);
                }
            }));
            assert!(result.is_ok(), "panicked on {name} mutation {offset}");
        }
        for range in qst_ranges(&original, hdr) {
            for offset in range.clone() {
                if (range.start + 26..range.start + 30).contains(&offset) {
                    continue;
                }
                let mut mutated = original.clone();
                mutated[offset] ^= 0x5a;
                rewrite_qst_and_outer_crc(&mut mutated, range.clone());
                let result = catch_unwind(AssertUnwindSafe(|| {
                    if hdr {
                        let _ = decode_hdr(&mutated, &limits);
                    } else {
                        let _ = decode_wide(&mutated, &limits);
                    }
                }));
                assert!(
                    result.is_ok(),
                    "panicked on rechecksummed {name} QST1 mutation {offset}"
                );
            }
        }
    }
}

#[test]
fn wide_sample_storage_variant_is_explicit() {
    let image = decode_wide(&fixture("wide-u16-10-boundary.qlic"), &Limits::default()).unwrap();
    assert!(matches!(image.samples, WideSamples::U16(_)));
}
