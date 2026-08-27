use crate::cursor::Cursor;
use crate::decode::decode_payload;
use crate::{Container, Error, LimitKind, Limits, Mode, Result, RgbaImage, decode_rgba};
use std::mem::size_of;

const KEY: u32 = 0;
const DUPLICATE: u32 = 1;
const RECTANGLE: u32 = 2;
const MOVE: u32 = 3;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AnimationFrame {
    pub image: RgbaImage,
    pub delay_ms: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Animation {
    pub width: u32,
    pub height: u32,
    pub loop_count: u32,
    pub frames: Vec<AnimationFrame>,
}

fn invalid(message: &'static str) -> Error {
    Error::InvalidAnimation(message)
}

fn reserve_frames(count: usize) -> Result<Vec<AnimationFrame>> {
    let mut frames = Vec::new();
    frames
        .try_reserve_exact(count)
        .map_err(|_| Error::AllocationFailed("animation frames"))?;
    Ok(frames)
}

fn copy_pixels(source: &[u8]) -> Result<Vec<u8>> {
    let mut pixels = Vec::new();
    pixels
        .try_reserve_exact(source.len())
        .map_err(|_| Error::AllocationFailed("animation frame pixels"))?;
    pixels.extend_from_slice(source);
    Ok(pixels)
}

fn frame_bytes(width: u32, height: u32) -> Result<usize> {
    let bytes = u64::from(width)
        .checked_mul(u64::from(height))
        .and_then(|value| value.checked_mul(4))
        .ok_or(Error::ArithmeticOverflow("animation frame bytes"))?;
    usize::try_from(bytes).map_err(|_| Error::ArithmeticOverflow("animation frame bytes"))
}

fn check_animation_allocation(
    frame_bytes: usize,
    frame_count: u32,
    extra_frames: u32,
    limits: &Limits,
) -> Result<()> {
    let frames = u64::from(frame_count)
        .checked_add(u64::from(extra_frames))
        .ok_or(Error::ArithmeticOverflow("animation working frames"))?;
    let pixels = (frame_bytes as u64)
        .checked_mul(frames)
        .ok_or(Error::ArithmeticOverflow("animation working pixels"))?;
    let table = (size_of::<AnimationFrame>() as u64)
        .checked_mul(u64::from(frame_count))
        .ok_or(Error::ArithmeticOverflow("animation frame table"))?;
    let actual = pixels
        .checked_add(table)
        .ok_or(Error::ArithmeticOverflow("animation working bytes"))?;
    if actual > limits.max_animation_bytes {
        return Err(Error::LimitExceeded {
            kind: LimitKind::AnimationBytes,
            limit: limits.max_animation_bytes,
            actual,
        });
    }
    Ok(())
}

fn nested_image(bytes: &[u8], width: u32, height: u32, limits: &Limits) -> Result<RgbaImage> {
    let nested = Container::parse(bytes, limits)?;
    if nested.header.mode == Mode::Animation {
        return Err(invalid("nested animation"));
    }
    if nested.header.width != width || nested.header.height != height {
        return Err(invalid("nested image dimensions differ"));
    }
    decode_rgba(bytes, limits)
}

fn parse_header<'a>(payload: &'a [u8], marker: &[u8; 4], count: u32) -> Result<(Cursor<'a>, u32)> {
    let mut cursor = Cursor::new(payload);
    if cursor.take(4)? != marker {
        return Err(invalid("invalid marker"));
    }
    if cursor.read_u32_le()? != count || count == 0 {
        return Err(invalid("frame count differs from outer header"));
    }
    let loop_count = cursor.read_u32_le()?;
    Ok((cursor, loop_count))
}

fn decode_qan1(
    payload: &[u8],
    width: u32,
    height: u32,
    count: u32,
    limits: &Limits,
) -> Result<Animation> {
    let (mut cursor, loop_count) = parse_header(payload, b"QAN1", count)?;
    check_animation_allocation(frame_bytes(width, height)?, count, 0, limits)?;
    let mut frames = reserve_frames(count as usize)?;
    for _ in 0..count {
        let delay_ms = cursor.read_u32_le()?;
        if cursor.read_u32_le()? != 0 {
            return Err(invalid("QAN1 frame flags are nonzero"));
        }
        let length = usize::try_from(cursor.read_u64_le()?)
            .map_err(|_| Error::ArithmeticOverflow("animation frame size"))?;
        let bytes = cursor.take(length)?;
        let image = nested_image(bytes, width, height, limits)?;
        frames.push(AnimationFrame {
            image,
            delay_ms: if delay_ms == 0 { 100 } else { delay_ms },
        });
    }
    if cursor.remaining() != 0 {
        return Err(invalid("trailing QAN1 bytes"));
    }
    Ok(Animation {
        width,
        height,
        loop_count,
        frames,
    })
}

fn checked_rectangle(
    canvas_width: u32,
    canvas_height: u32,
    x: u32,
    y: u32,
    width: u32,
    height: u32,
) -> Result<()> {
    if width == 0
        || height == 0
        || x >= canvas_width
        || y >= canvas_height
        || width > canvas_width - x
        || height > canvas_height - y
    {
        return Err(invalid("rectangle is outside the canvas"));
    }
    Ok(())
}

fn decode_qan2(
    payload: &[u8],
    canvas_width: u32,
    canvas_height: u32,
    count: u32,
    limits: &Limits,
) -> Result<Animation> {
    let (mut cursor, loop_count) = parse_header(payload, b"QAN2", count)?;
    let bytes_per_frame = frame_bytes(canvas_width, canvas_height)?;
    check_animation_allocation(bytes_per_frame, count, 1, limits)?;
    let mut frames = reserve_frames(count as usize)?;
    for index in 0..count {
        let delay = cursor.read_u32_le()?;
        let frame_type = cursor.read_u32_le()?;
        let delay_ms = if delay == 0 { 100 } else { delay };
        if index == 0 && frame_type != KEY {
            return Err(invalid("first QAN2 frame is not a key frame"));
        }
        if frame_type == DUPLICATE {
            let previous = frames
                .last()
                .ok_or_else(|| invalid("duplicate has no previous frame"))?;
            let image = RgbaImage {
                width: canvas_width,
                height: canvas_height,
                rgba: copy_pixels(&previous.image.rgba)?,
            };
            frames.push(AnimationFrame { image, delay_ms });
            continue;
        }
        if frame_type == MOVE {
            let source_x = cursor.read_u32_le()?;
            let source_y = cursor.read_u32_le()?;
            let destination_x = cursor.read_u32_le()?;
            let destination_y = cursor.read_u32_le()?;
            let width = cursor.read_u32_le()?;
            let height = cursor.read_u32_le()?;
            let clear = cursor.read_u32_le()?.to_le_bytes();
            checked_rectangle(
                canvas_width,
                canvas_height,
                source_x,
                source_y,
                width,
                height,
            )?;
            checked_rectangle(
                canvas_width,
                canvas_height,
                destination_x,
                destination_y,
                width,
                height,
            )?;
            let previous = frames
                .last()
                .ok_or_else(|| invalid("move has no previous frame"))?;
            let mut rgba = copy_pixels(&previous.image.rgba)?;
            let stride = canvas_width as usize * 4;
            for row in 0..height as usize {
                let start = (source_y as usize + row) * stride + source_x as usize * 4;
                for pixel in rgba[start..start + width as usize * 4].chunks_exact_mut(4) {
                    pixel.copy_from_slice(&clear);
                }
            }
            for row in 0..height as usize {
                let source = (source_y as usize + row) * stride + source_x as usize * 4;
                let target = (destination_y as usize + row) * stride + destination_x as usize * 4;
                rgba[target..target + width as usize * 4]
                    .copy_from_slice(&previous.image.rgba[source..source + width as usize * 4]);
            }
            frames.push(AnimationFrame {
                image: RgbaImage {
                    width: canvas_width,
                    height: canvas_height,
                    rgba,
                },
                delay_ms,
            });
            continue;
        }

        let (x, y, width, height) = if frame_type == RECTANGLE {
            let x = cursor.read_u32_le()?;
            let y = cursor.read_u32_le()?;
            let width = cursor.read_u32_le()?;
            let height = cursor.read_u32_le()?;
            checked_rectangle(canvas_width, canvas_height, x, y, width, height)?;
            (x, y, width, height)
        } else if frame_type == KEY {
            (0, 0, canvas_width, canvas_height)
        } else {
            return Err(invalid("unknown QAN2 frame type"));
        };
        let length = usize::try_from(cursor.read_u64_le()?)
            .map_err(|_| Error::ArithmeticOverflow("animation frame size"))?;
        let nested = cursor.take(length)?;
        let decoded = nested_image(nested, width, height, limits)?;
        if frame_type == KEY {
            frames.push(AnimationFrame {
                image: decoded,
                delay_ms,
            });
            continue;
        }
        let previous = frames
            .last()
            .ok_or_else(|| invalid("rectangle has no previous frame"))?;
        let mut rgba = copy_pixels(&previous.image.rgba)?;
        let destination_stride = canvas_width as usize * 4;
        let source_stride = width as usize * 4;
        for row in 0..height as usize {
            let target = (y as usize + row) * destination_stride + x as usize * 4;
            let source = row * source_stride;
            rgba[target..target + source_stride]
                .copy_from_slice(&decoded.rgba[source..source + source_stride]);
        }
        frames.push(AnimationFrame {
            image: RgbaImage {
                width: canvas_width,
                height: canvas_height,
                rgba,
            },
            delay_ms,
        });
    }
    if cursor.remaining() != 0 {
        return Err(invalid("trailing QAN2 bytes"));
    }
    Ok(Animation {
        width: canvas_width,
        height: canvas_height,
        loop_count,
        frames,
    })
}

pub fn decode_animation(bytes: &[u8], limits: &Limits) -> Result<Animation> {
    let container = Container::parse(bytes, limits)?;
    if container.header.mode != Mode::Animation {
        let image = decode_rgba(bytes, limits)?;
        let width = image.width;
        let height = image.height;
        let mut frames = reserve_frames(1)?;
        frames.push(AnimationFrame { image, delay_ms: 0 });
        return Ok(Animation {
            width,
            height,
            loop_count: 0,
            frames,
        });
    }
    let owned = decode_payload(&container, limits)?;
    let payload = owned.as_deref().unwrap_or(container.payload);
    if payload.starts_with(b"QAN2") {
        decode_qan2(
            payload,
            container.header.width,
            container.header.height,
            container.header.palette_count,
            limits,
        )
    } else {
        decode_qan1(
            payload,
            container.header.width,
            container.header.height,
            container.header.palette_count,
            limits,
        )
    }
}
