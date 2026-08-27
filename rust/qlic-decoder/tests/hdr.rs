use qlic_decoder::{
    AlphaAssociation, Cicp, ColorAuthority, Container, Error, LimitKind, Limits, Mode, SampleType,
    crc32, parse_info, parse_qsw2,
};

#[derive(Clone)]
struct Chunk {
    tag: [u8; 4],
    flags: u32,
    payload: Vec<u8>,
}

fn chunk(tag: &[u8; 4], flags: u32, payload: &[u8]) -> Chunk {
    Chunk {
        tag: *tag,
        flags,
        payload: payload.to_vec(),
    }
}

fn qsw1(bits: u8, channels: u8) -> Vec<u8> {
    let slice_count = bits.div_ceil(8);
    let mut payload = Vec::new();
    payload.extend_from_slice(b"QSW1");
    payload.extend_from_slice(&[0, bits, channels, slice_count]);
    payload.extend_from_slice(&0x7856_3412_u32.to_le_bytes());
    payload.extend_from_slice(&0_u32.to_le_bytes());
    for _ in 0..slice_count {
        payload.extend_from_slice(&1_u64.to_le_bytes());
    }
    payload.extend(std::iter::repeat_n(0xa5, usize::from(slice_count)));
    payload
}

fn cicp() -> Vec<u8> {
    [9_u16, 16, 9]
        .into_iter()
        .flat_map(u16::to_le_bytes)
        .chain([1, 0])
        .collect()
}

fn mastering_display() -> Vec<u8> {
    let mut payload = Vec::new();
    for value in [10_u16, 20, 30, 40, 50, 60, 70, 80] {
        payload.extend_from_slice(&value.to_le_bytes());
    }
    payload.extend_from_slice(&10_000_u32.to_le_bytes());
    payload.extend_from_slice(&50_u32.to_le_bytes());
    payload
}

fn content_light() -> Vec<u8> {
    let mut payload = Vec::new();
    payload.extend_from_slice(&1_000_u16.to_le_bytes());
    payload.extend_from_slice(&400_u16.to_le_bytes());
    payload
}

fn qsw2_file(
    width: u32,
    height: u32,
    bits: u8,
    channels: u8,
    alpha: u8,
    authority: u8,
    chunks: &[Chunk],
) -> Vec<u8> {
    let metadata_bytes = chunks
        .iter()
        .filter(|chunk| chunk.tag != *b"PIXL")
        .map(|chunk| chunk.payload.len() as u64)
        .sum::<u64>();
    let pixel_bytes = chunks
        .iter()
        .find(|chunk| chunk.tag == *b"PIXL")
        .map_or(1, |chunk| chunk.payload.len() as u64);

    let mut payload = Vec::new();
    payload.extend_from_slice(b"QSW2");
    payload.extend_from_slice(&[1, 1, bits, channels, alpha, authority]);
    payload.extend_from_slice(&0_u16.to_le_bytes());
    payload.extend_from_slice(&(chunks.len() as u32).to_le_bytes());
    payload.extend_from_slice(&metadata_bytes.to_le_bytes());
    payload.extend_from_slice(&pixel_bytes.to_le_bytes());
    for chunk in chunks {
        payload.extend_from_slice(&chunk.tag);
        payload.extend_from_slice(&chunk.flags.to_le_bytes());
        payload.extend_from_slice(&(chunk.payload.len() as u64).to_le_bytes());
        payload.extend_from_slice(&chunk.payload);
    }

    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&width.to_le_bytes());
    file.extend_from_slice(&height.to_le_bytes());
    file.extend_from_slice(&[20, 0, bits, 0x80]);
    file.extend_from_slice(&u32::from(channels).to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(&payload);
    let checksum = crc32(&file);
    file.extend_from_slice(&checksum.to_le_bytes());
    file
}

fn rewrite_crc(file: &mut [u8]) {
    let end = file.len() - 4;
    let checksum = crc32(&file[..end]).to_le_bytes();
    file[end..].copy_from_slice(&checksum);
}

fn parse(file: &[u8]) -> qlic_decoder::Qsw2Descriptor<'_> {
    let container = Container::parse(file, &Limits::default()).unwrap();
    assert_eq!(container.header.mode, Mode::HdrWide);
    parse_qsw2(&container, &Limits::default()).unwrap()
}

#[test]
fn parses_qsw2_metadata_and_exposes_hdr_info() {
    let pixels = qsw1(17, 3);
    let chunks = [
        chunk(b"ICCP", 0, b"icc"),
        chunk(b"CICP", 0, &cicp()),
        chunk(b"MDCV", 0, &mastering_display()),
        chunk(b"CLLI", 0, &content_light()),
        chunk(b"NOTE", 0, b"aux"),
        chunk(b"PIXL", 1, &pixels),
    ];
    let file = qsw2_file(2, 3, 17, 3, 0, 3, &chunks);
    let descriptor = parse(&file);

    assert_eq!((descriptor.width, descriptor.height), (2, 3));
    assert_eq!(descriptor.sample_type, SampleType::UnsignedInteger);
    assert_eq!(descriptor.alpha_association, AlphaAssociation::None);
    assert_eq!(descriptor.color_authority, ColorAuthority::IccPreferred);
    assert_eq!(descriptor.icc, Some(b"icc".as_slice()));
    assert_eq!(
        descriptor.cicp,
        Some(Cicp {
            color_primaries: 9,
            transfer_characteristics: 16,
            matrix_coefficients: 9,
            full_range: true,
        })
    );
    assert_eq!(descriptor.metadata_bytes, 42);
    assert_eq!(descriptor.pixels.sample_count, 18);
    assert_eq!(descriptor.pixels.decoded_bytes, 72);
    let mastering = descriptor.mastering_display.unwrap();
    assert_eq!(mastering.primary_x, [10, 30, 50]);
    assert_eq!(mastering.primary_y, [20, 40, 60]);
    assert_eq!(
        (mastering.max_luminance, mastering.min_luminance),
        (10_000, 50)
    );
    let light = descriptor.content_light.unwrap();
    assert_eq!((light.max_cll, light.max_fall), (1_000, 400));

    let info = parse_info(&file, &Limits::default()).unwrap();
    assert_eq!((info.width, info.height, info.channels), (2, 3, 3));
    assert_eq!(info.bits_per_sample, 17);
    assert_eq!(info.sample_type, Some(SampleType::UnsignedInteger));
    assert_eq!(info.alpha_association, Some(AlphaAssociation::None));
    assert_eq!(info.color_authority, Some(ColorAuthority::IccPreferred));
    assert!(info.has_icc && info.has_cicp);
    assert!(info.has_mastering_display && info.has_content_light);
}

#[test]
fn accepts_required_alpha_modes_and_ancillary_unknown_chunks() {
    for alpha in [1, 2] {
        let pixels = qsw1(12, 4);
        let chunks = [
            chunk(b"note", 0, b"retained ancillary data"),
            chunk(b"PIXL", 1, &pixels),
        ];
        let file = qsw2_file(1, 1, 12, 4, alpha, 0, &chunks);
        let descriptor = parse(&file);
        assert_eq!(
            descriptor.alpha_association,
            if alpha == 1 {
                AlphaAssociation::Straight
            } else {
                AlphaAssociation::Premultiplied
            }
        );
        assert_eq!(descriptor.metadata.len(), 1);
        assert_eq!(descriptor.metadata[0].tag, *b"note");
        assert_eq!(descriptor.metadata[0].data, b"retained ancillary data");
    }
}

#[test]
fn rejects_unknown_critical_and_bad_known_chunk_flags() {
    let pixels = qsw1(10, 1);
    let critical = [chunk(b"FUTR", 1, b"future"), chunk(b"PIXL", 1, &pixels)];
    let file = qsw2_file(1, 1, 10, 1, 0, 0, &critical);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert_eq!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::UnsupportedCriticalQsw2Chunk(*b"FUTR"))
    );

    let bad_known = [chunk(b"ICCP", 1, b"icc"), chunk(b"PIXL", 1, &pixels)];
    let file = qsw2_file(1, 1, 10, 1, 0, 1, &bad_known);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid ICCP chunk"))
    ));
}

#[test]
fn rejects_duplicate_known_chunks_and_missing_or_duplicate_pixels() {
    let pixels = qsw1(10, 1);
    let duplicate_icc = [
        chunk(b"ICCP", 0, b"one"),
        chunk(b"ICCP", 0, b"two"),
        chunk(b"PIXL", 1, &pixels),
    ];
    let file = qsw2_file(1, 1, 10, 1, 0, 1, &duplicate_icc);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid ICCP chunk"))
    ));

    let duplicate_pixels = [chunk(b"PIXL", 1, &pixels), chunk(b"PIXL", 1, &pixels)];
    let file = qsw2_file(1, 1, 10, 1, 0, 0, &duplicate_pixels);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid PIXL chunk"))
    ));

    let no_pixels = [chunk(b"note", 0, b"metadata")];
    let file = qsw2_file(1, 1, 10, 1, 0, 0, &no_pixels);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("missing PIXL chunk"))
    ));
}

#[test]
fn enforces_alpha_and_color_authority_consistency() {
    let rgb_pixels = qsw1(12, 3);
    let rgb_chunks = [chunk(b"PIXL", 1, &rgb_pixels)];
    let file = qsw2_file(1, 1, 12, 3, 1, 0, &rgb_chunks);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid alpha association"))
    ));

    let rgba_pixels = qsw1(12, 4);
    let rgba_chunks = [chunk(b"PIXL", 1, &rgba_pixels)];
    let file = qsw2_file(1, 1, 12, 4, 0, 0, &rgba_chunks);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid alpha association"))
    ));

    let wrong_authority = [chunk(b"ICCP", 0, b"icc"), chunk(b"PIXL", 1, &rgb_pixels)];
    let file = qsw2_file(1, 1, 12, 3, 0, 2, &wrong_authority);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2(
            "color authority does not match ICC/CICP presence"
        ))
    ));
}

#[test]
fn enforces_chunk_and_metadata_limits_before_pixel_decode() {
    let pixels = qsw1(12, 3);
    let chunks = [chunk(b"note", 0, b"metadata"), chunk(b"PIXL", 1, &pixels)];
    let file = qsw2_file(1, 1, 12, 3, 0, 0, &chunks);
    let container = Container::parse(&file, &Limits::default()).unwrap();

    let chunk_limits = Limits {
        max_chunks: 1,
        ..Limits::default()
    };
    assert!(matches!(
        parse_qsw2(&container, &chunk_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Chunks,
            ..
        })
    ));

    let metadata_limits = Limits {
        max_metadata_bytes: 7,
        ..Limits::default()
    };
    assert!(matches!(
        parse_qsw2(&container, &metadata_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::MetadataBytes,
            ..
        })
    ));

    let mut underdeclared = file;
    underdeclared[44..52].copy_from_slice(&7_u64.to_le_bytes());
    rewrite_crc(&mut underdeclared);
    let container = Container::parse(&underdeclared, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &metadata_limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::MetadataBytes,
            actual: 8,
            ..
        })
    ));
}

#[test]
fn rejects_invalid_metadata_values_and_embedded_qsw1_shape() {
    let pixels = qsw1(12, 3);
    let mut bad_cicp = cicp();
    bad_cicp[6] = 2;
    let chunks = [chunk(b"CICP", 0, &bad_cicp), chunk(b"PIXL", 1, &pixels)];
    let file = qsw2_file(1, 1, 12, 3, 0, 2, &chunks);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid CICP chunk"))
    ));

    let wrong_shape = qsw1(13, 3);
    let chunks = [chunk(b"PIXL", 1, &wrong_shape)];
    let file = qsw2_file(1, 1, 12, 3, 0, 0, &chunks);
    let container = Container::parse(&file, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid embedded QSW1 shape"))
    ));
}

#[test]
fn rejects_declared_size_mismatches_bad_flags_and_trailing_chunks() {
    let pixels = qsw1(12, 3);
    let chunks = [chunk(b"note", 0, b"metadata"), chunk(b"PIXL", 1, &pixels)];
    let original = qsw2_file(1, 1, 12, 3, 0, 0, &chunks);

    let mut metadata_mismatch = original.clone();
    metadata_mismatch[44..52].copy_from_slice(&7_u64.to_le_bytes());
    rewrite_crc(&mut metadata_mismatch);
    let container = Container::parse(&metadata_mismatch, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("declared metadata size mismatch"))
    ));

    let mut pixel_mismatch = original.clone();
    pixel_mismatch[52..60].copy_from_slice(&1_u64.to_le_bytes());
    rewrite_crc(&mut pixel_mismatch);
    let container = Container::parse(&pixel_mismatch, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid PIXL chunk"))
    ));

    let mut bad_flags = original.clone();
    bad_flags[64..68].copy_from_slice(&2_u32.to_le_bytes());
    rewrite_crc(&mut bad_flags);
    let container = Container::parse(&bad_flags, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("invalid chunk flags"))
    ));

    let mut trailing_chunk = original;
    trailing_chunk[40..44].copy_from_slice(&1_u32.to_le_bytes());
    rewrite_crc(&mut trailing_chunk);
    let container = Container::parse(&trailing_chunk, &Limits::default()).unwrap();
    assert!(matches!(
        parse_qsw2(&container, &Limits::default()),
        Err(Error::InvalidQsw2("trailing data after chunks"))
    ));
}
