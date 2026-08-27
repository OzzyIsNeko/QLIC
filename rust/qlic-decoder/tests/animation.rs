use std::fs;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::path::{Path, PathBuf};

use qlic_decoder::{Error, LimitKind, Limits, crc32, decode_animation};

fn fixture(name: &str) -> Vec<u8> {
    let directory: PathBuf = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/fixtures");
    fs::read(directory.join(name)).unwrap()
}

fn finish(mut file: Vec<u8>) -> Vec<u8> {
    file.extend_from_slice(&crc32(&file).to_le_bytes());
    file
}

fn stored_file(
    width: u32,
    height: u32,
    mode: u8,
    transform: u8,
    count: u32,
    payload: &[u8],
) -> Vec<u8> {
    let mut file = Vec::new();
    file.extend_from_slice(b"QLIC");
    file.extend_from_slice(&width.to_le_bytes());
    file.extend_from_slice(&height.to_le_bytes());
    file.extend_from_slice(&[mode, transform, 0, 0x80]);
    file.extend_from_slice(&count.to_le_bytes());
    file.extend_from_slice(&(payload.len() as u64).to_le_bytes());
    file.extend_from_slice(payload);
    finish(file)
}

fn rgba(width: u32, height: u32, pixels: &[[u8; 4]]) -> Vec<u8> {
    let mut payload = Vec::new();
    for pixel in pixels {
        payload.extend_from_slice(pixel);
    }
    assert_eq!(payload.len(), width as usize * height as usize * 4);
    stored_file(width, height, 4, 2, 0, &payload)
}

fn qan2_file(width: u32, height: u32, count: u32, payload: &[u8]) -> Vec<u8> {
    stored_file(width, height, 17, 0, count, payload)
}

fn push_frame_header(payload: &mut Vec<u8>, delay: u32, frame_type: u32) {
    payload.extend_from_slice(&delay.to_le_bytes());
    payload.extend_from_slice(&frame_type.to_le_bytes());
}

fn push_nested(payload: &mut Vec<u8>, nested: &[u8]) {
    payload.extend_from_slice(&(nested.len() as u64).to_le_bytes());
    payload.extend_from_slice(nested);
}

fn synthetic_qan2() -> Vec<u8> {
    let a = [1, 2, 3, 4];
    let b = [5, 6, 7, 8];
    let c = [9, 10, 11, 12];
    let d = [13, 14, 15, 16];
    let e = [21, 22, 23, 24];
    let f = [25, 26, 27, 28];
    let clear = [31, 32, 33, 34];
    let key = rgba(2, 2, &[a, b, c, d]);
    let patch = rgba(1, 2, &[e, f]);

    let mut payload = Vec::new();
    payload.extend_from_slice(b"QAN2");
    payload.extend_from_slice(&4_u32.to_le_bytes());
    payload.extend_from_slice(&3_u32.to_le_bytes());

    push_frame_header(&mut payload, 10, 0);
    push_nested(&mut payload, &key);

    push_frame_header(&mut payload, 0, 1);

    push_frame_header(&mut payload, 30, 2);
    for value in [1_u32, 0, 1, 2] {
        payload.extend_from_slice(&value.to_le_bytes());
    }
    push_nested(&mut payload, &patch);

    push_frame_header(&mut payload, 40, 3);
    for value in [1_u32, 0, 0, 0, 1, 2, u32::from_le_bytes(clear)] {
        payload.extend_from_slice(&value.to_le_bytes());
    }

    qan2_file(2, 2, 4, &payload)
}

fn rewrite_outer_crc(file: &mut [u8]) {
    let footer = file.len() - 4;
    let checksum = crc32(&file[..footer]).to_le_bytes();
    file[footer..].copy_from_slice(&checksum);
}

#[test]
fn decodes_retained_qan1_and_treats_a_still_as_one_frame() {
    let animation = decode_animation(&fixture("animation.qlic"), &Limits::default()).unwrap();
    assert_eq!((animation.width, animation.height), (2, 2));
    assert_eq!(animation.loop_count, 0);
    assert_eq!(animation.frames.len(), 2);
    assert_eq!(animation.frames[0].delay_ms, 40);
    assert_eq!(animation.frames[1].delay_ms, 70);
    assert_eq!(crc32(&animation.frames[0].image.rgba), 0xbc5a_ab1c);
    assert_eq!(crc32(&animation.frames[1].image.rgba), 0xd653_8a42);

    let still = decode_animation(&fixture("native.qlic"), &Limits::default()).unwrap();
    assert_eq!((still.width, still.height), (64, 64));
    assert_eq!(still.loop_count, 0);
    assert_eq!(still.frames.len(), 1);
    assert_eq!(still.frames[0].delay_ms, 0);
    assert_eq!(crc32(&still.frames[0].image.rgba), 0xb385_d194);
}

#[test]
fn decodes_qan2_key_duplicate_rectangle_and_move_exactly() {
    let animation = decode_animation(&synthetic_qan2(), &Limits::default()).unwrap();
    assert_eq!((animation.width, animation.height), (2, 2));
    assert_eq!(animation.loop_count, 3);
    assert_eq!(
        animation
            .frames
            .iter()
            .map(|frame| frame.delay_ms)
            .collect::<Vec<_>>(),
        [10, 100, 30, 40]
    );
    let key = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
    let rectangle = [1, 2, 3, 4, 21, 22, 23, 24, 9, 10, 11, 12, 25, 26, 27, 28];
    let moved = [
        21, 22, 23, 24, 31, 32, 33, 34, 25, 26, 27, 28, 31, 32, 33, 34,
    ];
    assert_eq!(animation.frames[0].image.rgba, key);
    assert_eq!(animation.frames[1].image.rgba, key);
    assert_eq!(animation.frames[2].image.rgba, rectangle);
    assert_eq!(animation.frames[3].image.rgba, moved);
}

#[test]
fn rejects_invalid_qan2_programs_and_resource_caps() {
    let original = synthetic_qan2();

    let mut first_duplicate = original.clone();
    first_duplicate[28 + 12 + 4..28 + 12 + 8].copy_from_slice(&1_u32.to_le_bytes());
    rewrite_outer_crc(&mut first_duplicate);
    assert!(matches!(
        decode_animation(&first_duplicate, &Limits::default()),
        Err(Error::InvalidAnimation(
            "first QAN2 frame is not a key frame"
        ))
    ));

    let mut invalid_rectangle = original.clone();
    invalid_rectangle[120..124].copy_from_slice(&2_u32.to_le_bytes());
    rewrite_outer_crc(&mut invalid_rectangle);
    assert!(matches!(
        decode_animation(&invalid_rectangle, &Limits::default()),
        Err(Error::InvalidAnimation("rectangle is outside the canvas"))
    ));

    let mut invalid_move = original.clone();
    invalid_move[208..212].copy_from_slice(&3_u32.to_le_bytes());
    rewrite_outer_crc(&mut invalid_move);
    assert!(matches!(
        decode_animation(&invalid_move, &Limits::default()),
        Err(Error::InvalidAnimation("rectangle is outside the canvas"))
    ));

    let payload_end = original.len() - 4;
    let mut trailing_payload = original[28..payload_end].to_vec();
    trailing_payload.push(0);
    assert!(matches!(
        decode_animation(&qan2_file(2, 2, 4, &trailing_payload), &Limits::default()),
        Err(Error::InvalidAnimation("trailing QAN2 bytes"))
    ));

    let limits = Limits {
        max_frames: 3,
        ..Limits::default()
    };
    assert!(matches!(
        decode_animation(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::Frames,
            ..
        })
    ));

    let limits = Limits {
        max_animation_bytes: 4 * 16,
        ..Limits::default()
    };
    assert!(matches!(
        decode_animation(&original, &limits),
        Err(Error::LimitExceeded {
            kind: LimitKind::AnimationBytes,
            ..
        })
    ));

    let key = rgba(1, 1, &[[1, 2, 3, 4]]);
    let mut nested_payload = Vec::new();
    nested_payload.extend_from_slice(b"QAN2");
    nested_payload.extend_from_slice(&1_u32.to_le_bytes());
    nested_payload.extend_from_slice(&0_u32.to_le_bytes());
    push_frame_header(&mut nested_payload, 1, 0);
    push_nested(&mut nested_payload, &key);
    let nested_animation = qan2_file(1, 1, 1, &nested_payload);
    let mut outer_payload = Vec::new();
    outer_payload.extend_from_slice(b"QAN2");
    outer_payload.extend_from_slice(&1_u32.to_le_bytes());
    outer_payload.extend_from_slice(&0_u32.to_le_bytes());
    push_frame_header(&mut outer_payload, 1, 0);
    push_nested(&mut outer_payload, &nested_animation);
    assert!(matches!(
        decode_animation(&qan2_file(1, 1, 1, &outer_payload), &Limits::default()),
        Err(Error::InvalidAnimation("nested animation"))
    ));
}

#[test]
fn malformed_animation_bytes_never_panic() {
    let original = synthetic_qan2();
    for end in 0..original.len() {
        let result = catch_unwind(AssertUnwindSafe(|| {
            decode_animation(&original[..end], &Limits::default())
        }));
        assert!(result.is_ok(), "panic on prefix {end}");
        assert!(result.unwrap().is_err(), "accepted prefix {end}");
    }

    for index in 0..original.len() - 4 {
        let mut changed = original.clone();
        changed[index] ^= 0x80;
        rewrite_outer_crc(&mut changed);
        let result = catch_unwind(AssertUnwindSafe(|| {
            decode_animation(&changed, &Limits::default())
        }));
        assert!(result.is_ok(), "panic on byte {index}");
    }
}
