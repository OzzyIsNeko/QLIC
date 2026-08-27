use std::fs;
use std::path::{Path, PathBuf};

use qlic_decoder::{
    Codec, Container, Error, LimitKind, Limits, crc32, decompress_lzms, decompress_lzms_into,
};

fn fixture_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures")
}

fn lzms_fixture(name: &str) -> (Vec<u8>, usize) {
    let file = fs::read(fixture_dir().join(name)).unwrap();
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert_eq!(container.header.codec, Codec::Lzms, "{name}");
    (
        container.payload.to_vec(),
        usize::try_from(container.header.payload_size).unwrap(),
    )
}

fn stored_wrapper(bytes: &[u8], block_size: usize) -> Vec<u8> {
    assert!(block_size != 0);
    let mut wrapper = Vec::new();
    wrapper.extend_from_slice(&[0x0a, 0x51, 0xe5, 0xc0, 0x18, 0x00, 0, 0]);
    wrapper.extend_from_slice(&(bytes.len() as u64).to_le_bytes());
    wrapper.extend_from_slice(&(block_size as u64).to_le_bytes());
    for block in bytes.chunks(block_size) {
        wrapper.extend_from_slice(&(block.len() as u32).to_le_bytes());
        wrapper.extend_from_slice(block);
    }
    wrapper
}

#[test]
fn decodes_stored_single_and_multi_block_wrappers() {
    let expected = b"safe Rust LZMS stored-block vector";
    for block_size in [expected.len(), 7] {
        let wrapper = stored_wrapper(expected, block_size);
        assert_eq!(
            decompress_lzms(&wrapper, expected.len(), &Limits::default()).unwrap(),
            expected
        );
        let mut output = vec![0; expected.len()];
        decompress_lzms_into(&wrapper, &mut output, &Limits::default()).unwrap();
        assert_eq!(output, expected);
    }
}

#[test]
fn decodes_retained_production_lzms_payloads() {
    // Sizes and CRCs were cross-checked against Windows Compression API LZMS,
    // which the production C compatibility test also uses as its oracle.
    let vectors = [
        ("cpalette-lzms.qlic", 32_776, 0xe868_02fe),
        ("gray-model-lzms.qlic", 98_948, 0x26a5_c97f),
        ("planar-med-lzms.qlic", 18, 0x39e2_064c),
        ("rgb-lzms.qlic", 6_939, 0xfb25_2055),
        ("tile-palette-lzms.qlic", 108_201, 0xad49_e57b),
    ];
    let mut discovered = fs::read_dir(fixture_dir())
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .filter(|path| {
            path.extension()
                .is_some_and(|extension| extension == "qlic")
        })
        .filter_map(|path| {
            let file = fs::read(&path).unwrap();
            let container = Container::parse(&file, &Limits::default()).unwrap();
            (container.header.codec == Codec::Lzms)
                .then(|| path.file_name().unwrap().to_string_lossy().into_owned())
        })
        .collect::<Vec<_>>();
    discovered.sort();
    let mut expected_names = vectors.map(|vector| vector.0);
    expected_names.sort();
    assert_eq!(discovered, expected_names);

    for (name, known_size, known_crc) in vectors {
        let (compressed, expected_size) = lzms_fixture(name);
        let decoded = decompress_lzms(&compressed, expected_size, &Limits::default()).unwrap();
        assert_eq!(decoded.len(), known_size, "{name}");
        assert_eq!(crc32(&decoded), known_crc, "{name}");
    }
}

#[test]
fn rejects_structural_corruption_and_resource_limit_violations() {
    let expected = b"bounded output";
    let original = stored_wrapper(expected, expected.len());

    let mut invalid_signature = original.clone();
    invalid_signature[0] ^= 1;
    assert!(matches!(
        decompress_lzms(&invalid_signature, expected.len(), &Limits::default()),
        Err(Error::InvalidLzms("invalid wrapper signature"))
    ));

    let mut invalid_output = original.clone();
    invalid_output[8..16].copy_from_slice(&1_u64.to_le_bytes());
    assert!(matches!(
        decompress_lzms(&invalid_output, expected.len(), &Limits::default()),
        Err(Error::InvalidLzms("declared output size mismatch"))
    ));

    let mut zero_block = original.clone();
    zero_block[16..24].fill(0);
    assert!(matches!(
        decompress_lzms(&zero_block, expected.len(), &Limits::default()),
        Err(Error::InvalidLzms("invalid block size"))
    ));

    let mut trailing = original.clone();
    trailing.push(0);
    assert!(matches!(
        decompress_lzms(&trailing, expected.len(), &Limits::default()),
        Err(Error::InvalidLzms("trailing wrapper data"))
    ));

    let mut odd_raw_block = Vec::new();
    odd_raw_block.extend_from_slice(&[0x0a, 0x51, 0xe5, 0xc0, 0x18, 0x00, 0, 0]);
    odd_raw_block.extend_from_slice(&10_u64.to_le_bytes());
    odd_raw_block.extend_from_slice(&10_u64.to_le_bytes());
    odd_raw_block.extend_from_slice(&5_u32.to_le_bytes());
    odd_raw_block.extend_from_slice(&[0; 5]);
    assert!(matches!(
        decompress_lzms(&odd_raw_block, 10, &Limits::default()),
        Err(Error::InvalidLzms("invalid raw block size"))
    ));

    let input_limits = Limits {
        max_file_bytes: original.len() as u64 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decompress_lzms(&original, expected.len(), &input_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::FileBytes,
            ..
        })
    ));

    let output_limits = Limits {
        max_payload_bytes: expected.len() as u64 - 1,
        ..Limits::default()
    };
    assert!(matches!(
        decompress_lzms(&original, expected.len(), &output_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::PayloadBytes,
            ..
        })
    ));
}

#[test]
fn rejects_all_prefix_truncations_of_a_compressed_fixture() {
    let (compressed, expected_size) = lzms_fixture("cpalette-lzms.qlic");
    for length in 0..compressed.len() {
        assert!(
            decompress_lzms(&compressed[..length], expected_size, &Limits::default()).is_err(),
            "accepted LZMS prefix of {length} bytes"
        );
    }
}

#[test]
fn random_malformed_inputs_do_not_panic_or_allocate_unbounded_output() {
    let limits = Limits {
        max_file_bytes: 256,
        max_payload_bytes: 4_096,
        ..Limits::default()
    };
    let mut state = 0x6d2b_79f5_u32;
    let mut bytes = [0_u8; 256];
    for _ in 0..2_000 {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        let input_size = state as usize % bytes.len();
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        let output_size = state as usize % 4_096;
        for byte in &mut bytes[..input_size] {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            *byte = state as u8;
        }
        let _ = decompress_lzms(&bytes[..input_size], output_size, &limits);
    }

    // Valid wrapper framing around random even-length raw blocks reaches the
    // adaptive range/Huffman decoder instead of stopping at the signature.
    for _ in 0..500 {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        let output_size = 8 + state as usize % 249;
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        let maximum = (output_size - 1).min(64);
        let compressed_size = 4 + (state as usize % (maximum - 3));
        let compressed_size = compressed_size & !1;
        let mut wrapper = Vec::new();
        wrapper.extend_from_slice(&[0x0a, 0x51, 0xe5, 0xc0, 0x18, 0x00, 0, 0]);
        wrapper.extend_from_slice(&(output_size as u64).to_le_bytes());
        wrapper.extend_from_slice(&(output_size as u64).to_le_bytes());
        wrapper.extend_from_slice(&(compressed_size as u32).to_le_bytes());
        for _ in 0..compressed_size {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            wrapper.push(state as u8);
        }
        let _ = decompress_lzms(&wrapper, output_size, &limits);
    }
}
