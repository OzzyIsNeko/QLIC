// Safe Rust port of QLIC's portable clean-room LZMS decoder.
// The original implementation is MIT licensed; see third_party/lzms/LICENSE.

use crate::{Error, LimitKind, Limits, Result};

const REPS: usize = 3;
const MAIN_PROBS: usize = 16;
const MATCH_PROBS: usize = 32;
const LZ_PROBS: usize = 64;
const REP_PROBS: usize = 64;
const PROB_BITS: u32 = 6;
const PROB_DENOMINATOR: u32 = 1 << PROB_BITS;
const INITIAL_PROB: u32 = 48;
const INITIAL_RECENT: u64 = 0x0000_0000_5555_5555;
const LITERAL_SYMBOLS: usize = 256;
const LENGTH_SYMBOLS: usize = 54;
const DELTA_POWER_SYMBOLS: usize = 8;
const MAX_OFFSET_SYMBOLS: usize = 799;
const MAX_CODEWORD_BITS: usize = 15;
const TABLE_SIZE: usize = 1 << MAX_CODEWORD_BITS;
const SYMBOL_BITS: u32 = 10;
const SYMBOL_MASK: u32 = (1 << SYMBOL_BITS) - 1;
const FREQ_MASK: u32 = !SYMBOL_MASK;
const X86_WINDOW: i32 = 65_535;
const X86_MAX_TRANSLATION: i32 = 1_023;
const SIGNATURE: [u8; 6] = [0x0a, 0x51, 0xe5, 0xc0, 0x18, 0x00];

const LENGTH_EXTRA: [u8; LENGTH_SYMBOLS] = [
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
    2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 6, 7, 8, 9, 10, 16, 30,
];

const OFFSET_EXTRA_COUNTS: [u16; 31] = [
    8, 0, 9, 7, 10, 15, 15, 20, 20, 30, 33, 40, 42, 45, 60, 73, 80, 85, 95, 105, 6, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1,
];

fn invalid(message: &'static str) -> Error {
    Error::InvalidLzms(message)
}

fn read_array<const LENGTH: usize>(
    bytes: &[u8],
    offset: usize,
    context: &'static str,
) -> Result<[u8; LENGTH]> {
    let end = offset
        .checked_add(LENGTH)
        .ok_or(Error::ArithmeticOverflow(context))?;
    let source = bytes
        .get(offset..end)
        .ok_or_else(|| invalid("truncated integer"))?;
    let mut output = [0_u8; LENGTH];
    for (target, &byte) in output.iter_mut().zip(source) {
        *target = byte;
    }
    Ok(output)
}

fn read_u16(bytes: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(read_array(
        bytes,
        offset,
        "LZMS 16-bit read",
    )?))
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(read_array(
        bytes,
        offset,
        "LZMS 32-bit read",
    )?))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(read_array(
        bytes,
        offset,
        "LZMS 64-bit read",
    )?))
}

fn filled_vec<T: Clone>(length: usize, value: T, context: &'static str) -> Result<Vec<T>> {
    let mut output = Vec::new();
    output
        .try_reserve_exact(length)
        .map_err(|_| Error::AllocationFailed(context))?;
    output.resize(length, value);
    Ok(output)
}

#[derive(Clone, Copy)]
struct Probability {
    zeros: u32,
    recent: u64,
}

impl Probability {
    const INITIAL: Self = Self {
        zeros: INITIAL_PROB,
        recent: INITIAL_RECENT,
    };

    fn value(self) -> u32 {
        let mut value = self.zeros;
        value = value.wrapping_add(value.wrapping_sub(1) >> 31);
        value.wrapping_sub(value >> PROB_BITS)
    }
}

struct RangeDecoder<'a> {
    range: u32,
    code: u32,
    source: &'a [u8],
    next: usize,
}

impl<'a> RangeDecoder<'a> {
    fn new(source: &'a [u8]) -> Result<Self> {
        Ok(Self {
            range: u32::MAX,
            code: (u32::from(read_u16(source, 0)?) << 16) | u32::from(read_u16(source, 2)?),
            source,
            next: 4,
        })
    }

    fn bit<const STATES: usize>(
        &mut self,
        state: &mut u32,
        probabilities: &mut [Probability; STATES],
    ) -> Result<u32> {
        let index = usize::try_from(*state).map_err(|_| invalid("invalid probability state"))?;
        let probability = probabilities
            .get_mut(index)
            .ok_or_else(|| invalid("invalid probability state"))?;
        *state = (*state << 1) & (STATES as u32 - 1);
        let chance = probability.value();
        if self.range & 0xffff_0000 == 0 {
            self.range <<= 16;
            self.code <<= 16;
            if self
                .next
                .checked_add(2)
                .is_some_and(|end| end <= self.source.len())
            {
                self.code |= u32::from(read_u16(self.source, self.next)?);
                self.next += 2;
            }
        }
        let boundary = (self.range >> PROB_BITS).wrapping_mul(chance);
        let bit = if self.code < boundary {
            self.range = boundary;
            0
        } else {
            self.range = self.range.wrapping_sub(boundary);
            self.code = self.code.wrapping_sub(boundary);
            *state |= 1;
            1
        };
        let delta = (probability.recent >> (PROB_DENOMINATOR - 1)) as i32 - bit as i32;
        probability.zeros = (probability.zeros as i32).wrapping_add(delta) as u32;
        probability.recent = (probability.recent << 1) | u64::from(bit);
        Ok(bit)
    }
}

struct BackwardBits<'a> {
    bits: u64,
    available: u32,
    source: &'a [u8],
    next: usize,
}

impl<'a> BackwardBits<'a> {
    const fn new(source: &'a [u8]) -> Self {
        Self {
            bits: 0,
            available: 0,
            source,
            next: source.len(),
        }
    }

    fn ensure(&mut self, count: u32) -> Result<()> {
        if self.available >= count {
            return Ok(());
        }
        let space = 64 - self.available;
        if self.next >= 2 {
            self.next -= 2;
            self.bits |= u64::from(read_u16(self.source, self.next)?) << (space - 16);
        }
        if self.next >= 2 {
            self.next -= 2;
            self.bits |= u64::from(read_u16(self.source, self.next)?) << (space - 32);
        }
        self.available += 32;
        Ok(())
    }

    fn peek(&self, count: u32) -> u32 {
        ((self.bits >> 1) >> (63 - count)) as u32
    }

    fn read(&mut self, count: u32) -> Result<u32> {
        if count == 0 {
            return Ok(0);
        }
        self.ensure(count)?;
        if self.available < count {
            return Err(invalid("backward bitstream underflow"));
        }
        let value = self.peek(count);
        self.bits <<= count;
        self.available -= count;
        Ok(value)
    }
}

struct Huffman {
    symbols: usize,
    rebuild_frequency: u32,
    until_rebuild: u32,
    frequencies: Vec<u32>,
    scratch: Vec<u32>,
    lengths: Vec<u8>,
    table: Vec<u16>,
}

impl Huffman {
    fn new(symbols: usize, rebuild_frequency: u32) -> Result<Self> {
        if symbols == 0 {
            return Ok(Self {
                symbols: 0,
                rebuild_frequency,
                until_rebuild: 0,
                frequencies: Vec::new(),
                scratch: Vec::new(),
                lengths: Vec::new(),
                table: Vec::new(),
            });
        }
        let mut value = Self {
            symbols,
            rebuild_frequency,
            until_rebuild: 0,
            frequencies: filled_vec(symbols, 1, "LZMS Huffman frequencies")?,
            scratch: filled_vec(symbols, 0, "LZMS Huffman scratch")?,
            lengths: filled_vec(symbols, 0, "LZMS Huffman lengths")?,
            table: filled_vec(TABLE_SIZE, 0, "LZMS Huffman table")?,
        };
        value.rebuild()?;
        Ok(value)
    }

    fn make_lengths(&mut self) -> Result<()> {
        let mut used = 0;
        for symbol in 0..self.symbols {
            if self.frequencies[symbol] == 0 {
                self.lengths[symbol] = 0;
                continue;
            }
            self.scratch[used] = symbol as u32 | (self.frequencies[symbol] << SYMBOL_BITS);
            used += 1;
        }
        self.scratch[..used].sort_unstable();
        if used == 0 {
            return Err(invalid("empty Huffman alphabet"));
        }
        if used == 1 {
            let symbol = (self.scratch[0] & SYMBOL_MASK) as usize;
            let other = if symbol != 0 { 0 } else { 1 };
            if other >= self.symbols {
                return Err(invalid("single-symbol Huffman alphabet"));
            }
            self.lengths[symbol] = 1;
            self.lengths[other] = 1;
            return Ok(());
        }

        build_tree(&mut self.scratch, used)?;
        let mut counts = [0_i32; MAX_CODEWORD_BITS + 2];
        counts[1] = 2;
        self.scratch[used - 2] &= SYMBOL_MASK;
        for node in (0..used - 2).rev() {
            let parent = (self.scratch[node] >> SYMBOL_BITS) as usize;
            let parent_value = *self
                .scratch
                .get(parent)
                .ok_or_else(|| invalid("invalid Huffman parent"))?;
            let depth = (parent_value >> SYMBOL_BITS) + 1;
            self.scratch[node] = (self.scratch[node] & SYMBOL_MASK) | (depth << SYMBOL_BITS);
            let mut length = depth as usize;
            if length >= MAX_CODEWORD_BITS {
                length = MAX_CODEWORD_BITS;
                while length != 0 && counts[length] == 0 {
                    length -= 1;
                }
                if length == 0 {
                    return Err(invalid("Huffman length overflow"));
                }
            }
            counts[length] -= 1;
            counts[length + 1] += 2;
        }
        let mut index = 0;
        for length in (1..=MAX_CODEWORD_BITS).rev() {
            for _ in 0..counts[length] {
                if index >= used {
                    return Err(invalid("invalid Huffman length count"));
                }
                let symbol = (self.scratch[index] & SYMBOL_MASK) as usize;
                self.lengths[symbol] = length as u8;
                index += 1;
            }
        }
        if index != used {
            return Err(invalid("incomplete Huffman lengths"));
        }
        Ok(())
    }

    fn build_table(&mut self) -> Result<()> {
        let mut counts = [0_usize; MAX_CODEWORD_BITS + 1];
        for &length in &self.lengths {
            let length = usize::from(length);
            if length > MAX_CODEWORD_BITS {
                return Err(invalid("Huffman codeword is too long"));
            }
            counts[length] += 1;
        }
        let mut positions = [0_usize; MAX_CODEWORD_BITS + 2];
        for length in 1..=MAX_CODEWORD_BITS {
            positions[length + 1] = positions[length]
                .checked_add(counts[length] << (MAX_CODEWORD_BITS - length))
                .ok_or(Error::ArithmeticOverflow("LZMS Huffman table positions"))?;
        }
        for symbol in 0..self.symbols {
            let length = usize::from(self.lengths[symbol]);
            if length == 0 {
                continue;
            }
            let start = positions[length];
            let end = start
                .checked_add(1 << (MAX_CODEWORD_BITS - length))
                .ok_or(Error::ArithmeticOverflow("LZMS Huffman table range"))?;
            if end > TABLE_SIZE {
                return Err(invalid("oversubscribed Huffman table"));
            }
            self.table[start..end].fill(symbol as u16);
            positions[length] = end;
        }
        if positions[MAX_CODEWORD_BITS + 1] != TABLE_SIZE {
            return Err(invalid("incomplete Huffman table"));
        }
        Ok(())
    }

    fn rebuild(&mut self) -> Result<()> {
        self.make_lengths()?;
        self.build_table()?;
        self.until_rebuild = self.rebuild_frequency;
        for frequency in &mut self.frequencies {
            *frequency = (*frequency >> 1) + 1;
        }
        Ok(())
    }

    fn decode(&mut self, bits: &mut BackwardBits<'_>) -> Result<u32> {
        if self.symbols == 0 {
            return Err(invalid("missing Huffman alphabet"));
        }
        bits.ensure(MAX_CODEWORD_BITS as u32)?;
        let index = bits.peek(MAX_CODEWORD_BITS as u32) as usize;
        let symbol = usize::from(
            *self
                .table
                .get(index)
                .ok_or_else(|| invalid("invalid Huffman lookup"))?,
        );
        let length = self.lengths.get(symbol).copied().unwrap_or(0);
        if symbol >= self.symbols || length == 0 {
            return Err(invalid("invalid Huffman symbol"));
        }
        if bits.available < u32::from(length) {
            return Err(invalid("truncated Huffman codeword"));
        }
        bits.bits <<= length;
        bits.available -= u32::from(length);
        self.frequencies[symbol] = self.frequencies[symbol].wrapping_add(1);
        self.until_rebuild = self
            .until_rebuild
            .checked_sub(1)
            .ok_or_else(|| invalid("invalid Huffman rebuild state"))?;
        if self.until_rebuild == 0 {
            self.rebuild()?;
        }
        Ok(symbol as u32)
    }
}

fn build_tree(tree: &mut [u32], symbols: usize) -> Result<()> {
    let last = symbols - 1;
    let mut leaf = 0;
    let mut queued = 0;
    let mut end = 0;
    loop {
        let frequency;
        if leaf < last
            && (queued == end || (tree[leaf + 1] & FREQ_MASK) <= (tree[queued] & FREQ_MASK))
        {
            frequency = (tree[leaf] & FREQ_MASK).wrapping_add(tree[leaf + 1] & FREQ_MASK);
            leaf += 2;
        } else if queued + 2 <= end
            && (leaf > last || (tree[queued + 1] & FREQ_MASK) < (tree[leaf] & FREQ_MASK))
        {
            frequency = (tree[queued] & FREQ_MASK).wrapping_add(tree[queued + 1] & FREQ_MASK);
            tree[queued] = ((end as u32) << SYMBOL_BITS) | (tree[queued] & SYMBOL_MASK);
            tree[queued + 1] = ((end as u32) << SYMBOL_BITS) | (tree[queued + 1] & SYMBOL_MASK);
            queued += 2;
        } else {
            if leaf > last || queued >= end {
                return Err(invalid("invalid Huffman tree"));
            }
            frequency = (tree[leaf] & FREQ_MASK).wrapping_add(tree[queued] & FREQ_MASK);
            tree[queued] = ((end as u32) << SYMBOL_BITS) | (tree[queued] & SYMBOL_MASK);
            leaf += 1;
            queued += 1;
        }
        tree[end] = frequency | (tree[end] & SYMBOL_MASK);
        end += 1;
        if end >= last {
            return Ok(());
        }
    }
}

struct Decoder {
    literal: Huffman,
    lz_offset: Huffman,
    length: Huffman,
    delta_offset: Huffman,
    delta_power: Huffman,
    main: [Probability; MAIN_PROBS],
    match_prob: [Probability; MATCH_PROBS],
    lz: [Probability; LZ_PROBS],
    delta: [Probability; LZ_PROBS],
    lz_rep: [[Probability; REP_PROBS]; REPS - 1],
    delta_rep: [[Probability; REP_PROBS]; REPS - 1],
    offset_extra: [u8; MAX_OFFSET_SYMBOLS],
    offset_base: [u32; MAX_OFFSET_SYMBOLS + 1],
    length_base: [u32; LENGTH_SYMBOLS + 1],
}

impl Decoder {
    fn new(output_size: usize) -> Result<Self> {
        let mut offset_extra = [0; MAX_OFFSET_SYMBOLS];
        let mut offset_base = [0; MAX_OFFSET_SYMBOLS + 1];
        let mut length_base = [0; LENGTH_SYMBOLS + 1];
        let offsets = make_slots(
            output_size,
            &mut offset_extra,
            &mut offset_base,
            &mut length_base,
        )?;
        if output_size >= 2 && offsets == 0 {
            return Err(invalid("could not construct offset slots"));
        }
        Ok(Self {
            literal: Huffman::new(LITERAL_SYMBOLS, 1024)?,
            lz_offset: Huffman::new(offsets, 1024)?,
            length: Huffman::new(LENGTH_SYMBOLS, 512)?,
            delta_offset: Huffman::new(offsets, 1024)?,
            delta_power: Huffman::new(DELTA_POWER_SYMBOLS, 512)?,
            main: [Probability::INITIAL; MAIN_PROBS],
            match_prob: [Probability::INITIAL; MATCH_PROBS],
            lz: [Probability::INITIAL; LZ_PROBS],
            delta: [Probability::INITIAL; LZ_PROBS],
            lz_rep: [[Probability::INITIAL; REP_PROBS]; REPS - 1],
            delta_rep: [[Probability::INITIAL; REP_PROBS]; REPS - 1],
            offset_extra,
            offset_base,
            length_base,
        })
    }

    fn decode_lz_offset(&mut self, bits: &mut BackwardBits<'_>) -> Result<u32> {
        let slot = self.lz_offset.decode(bits)? as usize;
        let base = *self
            .offset_base
            .get(slot)
            .ok_or_else(|| invalid("invalid LZ offset slot"))?;
        let extra = *self
            .offset_extra
            .get(slot)
            .ok_or_else(|| invalid("invalid LZ offset slot"))?;
        Ok(base.wrapping_add(bits.read(u32::from(extra))?))
    }

    fn decode_delta_offset(&mut self, bits: &mut BackwardBits<'_>) -> Result<u32> {
        let slot = self.delta_offset.decode(bits)? as usize;
        let base = *self
            .offset_base
            .get(slot)
            .ok_or_else(|| invalid("invalid delta offset slot"))?;
        let extra = *self
            .offset_extra
            .get(slot)
            .ok_or_else(|| invalid("invalid delta offset slot"))?;
        Ok(base.wrapping_add(bits.read(u32::from(extra))?))
    }

    fn decode_length(&mut self, bits: &mut BackwardBits<'_>) -> Result<u32> {
        let slot = self.length.decode(bits)? as usize;
        let base = *self
            .length_base
            .get(slot)
            .ok_or_else(|| invalid("invalid length slot"))?;
        let extra = *LENGTH_EXTRA
            .get(slot)
            .ok_or_else(|| invalid("invalid length slot"))?;
        Ok(base.wrapping_add(bits.read(u32::from(extra))?))
    }
}

fn make_slots(
    output_size: usize,
    offset_extra: &mut [u8; MAX_OFFSET_SYMBOLS],
    offset_base: &mut [u32; MAX_OFFSET_SYMBOLS + 1],
    length_base: &mut [u32; LENGTH_SYMBOLS + 1],
) -> Result<usize> {
    let mut base = 1_u32;
    let mut slot = 0;
    let mut needed = 0;
    let target = if output_size > 1 {
        (output_size - 1) as u32
    } else {
        0
    };
    for (bits, &count) in OFFSET_EXTRA_COUNTS.iter().enumerate() {
        for _ in 0..count {
            if slot >= MAX_OFFSET_SYMBOLS {
                return Err(invalid("too many LZMS offset slots"));
            }
            offset_extra[slot] = bits as u8;
            offset_base[slot] = base;
            let width = 1_u32 << bits;
            if needed == 0 && target >= base && target.wrapping_sub(base) < width {
                needed = slot + 1;
            }
            base = base.wrapping_add(width);
            slot += 1;
        }
    }
    offset_base[slot] = base;
    base = 1;
    for (index, &extra) in LENGTH_EXTRA.iter().enumerate() {
        length_base[index] = base;
        base = base.wrapping_add(1_u32 << extra);
    }
    length_base[LENGTH_SYMBOLS] = base;
    Ok(if output_size < 2 { 0 } else { needed })
}

fn raw_decompress(source: &[u8], destination: &mut [u8]) -> Result<()> {
    if source.len() & 1 != 0 || source.len() < 4 {
        return Err(invalid("invalid raw block size"));
    }
    let mut decoder = Decoder::new(destination.len())?;
    let mut range = RangeDecoder::new(source)?;
    let mut bits = BackwardBits::new(source);
    let mut recent_lz = [1_u32, 2, 3, 4];
    let mut recent_delta = [1_u64, 2, 3, 4];
    let mut main_state = 0;
    let mut match_state = 0;
    let mut lz_state = 0;
    let mut delta_state = 0;
    let mut lz_rep_states = [0_u32; REPS - 1];
    let mut delta_rep_states = [0_u32; REPS - 1];
    let mut previous = 0_u32;
    let mut position = 0_usize;

    while position < destination.len() {
        if range.bit(&mut main_state, &mut decoder.main)? == 0 {
            let symbol = decoder.literal.decode(&mut bits)?;
            destination[position] = symbol as u8;
            position += 1;
            previous = 0;
            continue;
        }
        if range.bit(&mut match_state, &mut decoder.match_prob)? == 0 {
            let offset;
            if range.bit(&mut lz_state, &mut decoder.lz)? == 0 {
                offset = decoder.decode_lz_offset(&mut bits)?;
                recent_lz[3] = recent_lz[2];
                recent_lz[2] = recent_lz[1];
                recent_lz[1] = recent_lz[0];
            } else {
                let adjustment = (previous & 1) as usize;
                if range.bit(&mut lz_rep_states[0], &mut decoder.lz_rep[0])? == 0 {
                    offset = recent_lz[adjustment];
                    recent_lz[adjustment] = recent_lz[0];
                } else if range.bit(&mut lz_rep_states[1], &mut decoder.lz_rep[1])? == 0 {
                    offset = recent_lz[1 + adjustment];
                    recent_lz[1 + adjustment] = recent_lz[1];
                    recent_lz[1] = recent_lz[0];
                } else {
                    offset = recent_lz[2 + adjustment];
                    recent_lz[2 + adjustment] = recent_lz[2];
                    recent_lz[2] = recent_lz[1];
                    recent_lz[1] = recent_lz[0];
                }
            }
            recent_lz[0] = offset;
            previous = 1;
            let length = decoder.decode_length(&mut bits)?;
            let length_usize = length as usize;
            let offset_usize = offset as usize;
            if offset == 0
                || length == 0
                || offset_usize > position
                || length_usize > destination.len() - position
            {
                return Err(invalid("invalid LZ match"));
            }
            let mut match_position = position - offset_usize;
            let mut remaining = length_usize;
            while remaining != 0 {
                destination[position] = destination[match_position];
                position += 1;
                match_position += 1;
                remaining -= 1;
            }
            continue;
        }

        let pair;
        if range.bit(&mut delta_state, &mut decoder.delta)? == 0 {
            let power = decoder.delta_power.decode(&mut bits)?;
            let offset = decoder.decode_delta_offset(&mut bits)?;
            if offset == 0 {
                return Err(invalid("invalid delta offset"));
            }
            pair = (u64::from(power) << 32) | u64::from(offset);
            recent_delta[3] = recent_delta[2];
            recent_delta[2] = recent_delta[1];
            recent_delta[1] = recent_delta[0];
        } else {
            let adjustment = (previous >> 1) as usize;
            if range.bit(&mut delta_rep_states[0], &mut decoder.delta_rep[0])? == 0 {
                pair = recent_delta[adjustment];
                recent_delta[adjustment] = recent_delta[0];
            } else if range.bit(&mut delta_rep_states[1], &mut decoder.delta_rep[1])? == 0 {
                pair = recent_delta[1 + adjustment];
                recent_delta[1 + adjustment] = recent_delta[1];
                recent_delta[1] = recent_delta[0];
            } else {
                pair = recent_delta[2 + adjustment];
                recent_delta[2 + adjustment] = recent_delta[2];
                recent_delta[2] = recent_delta[1];
                recent_delta[1] = recent_delta[0];
            }
        }
        recent_delta[0] = pair;
        previous = 2;
        let length = decoder.decode_length(&mut bits)?;
        let power = (pair >> 32) as u32;
        let raw_offset = pair as u32;
        if power >= 32 {
            return Err(invalid("invalid delta power"));
        }
        let span = 1_u32 << power;
        let offset = raw_offset.wrapping_shl(power);
        let offset_end = offset.wrapping_add(span);
        let length_usize = length as usize;
        if length == 0
            || offset == 0
            || (offset >> power) != raw_offset
            || offset_end < offset
            || offset_end as usize > position
            || length_usize > destination.len() - position
        {
            return Err(invalid("invalid delta match"));
        }
        let span = span as usize;
        let mut match_position = position - offset as usize;
        let mut remaining = length_usize;
        while remaining != 0 {
            destination[position] = destination[match_position]
                .wrapping_add(destination[position - span])
                .wrapping_sub(destination[match_position - span]);
            position += 1;
            match_position += 1;
            remaining -= 1;
        }
    }
    x86_filter(destination)
}

fn is_opcode(value: u8) -> bool {
    matches!(value, 0x48 | 0x4c | 0xe8 | 0xe9 | 0xf0 | 0xff)
}

fn translate_x86(
    data: &mut [u8],
    mut position: usize,
    last_x86: &mut i32,
    last_targets: &mut [i32],
) -> Result<usize> {
    let mut max_offset = X86_MAX_TRANSLATION;
    let opcode_bytes;
    if data[position] >= 0xf0 {
        if data[position] & 0x0f != 0 {
            if data[position + 1] != 0x15 {
                return Ok(position + 1);
            }
            opcode_bytes = 2;
        } else {
            if data[position + 1] != 0x83 || data[position + 2] != 0x05 {
                return Ok(position + 1);
            }
            opcode_bytes = 3;
        }
    } else if data[position] <= 0x4c {
        if data[position + 2] & 0x07 != 0x05
            || (data[position + 1] != 0x8d
                && (data[position + 1] != 0x8b
                    || data[position] & 0x04 != 0
                    || data[position + 2] & 0xf0 != 0))
        {
            return Ok(position + 1);
        }
        opcode_bytes = 3;
    } else {
        if data[position] & 1 != 0 {
            return Ok(position + 5);
        }
        opcode_bytes = 1;
        max_offset >>= 1;
    }
    let instruction = position;
    position += opcode_bytes;
    let instruction_i32 = instruction as u32 as i32;
    if instruction_i32.wrapping_sub(*last_x86) <= max_offset {
        let value = read_u32(data, position)?.wrapping_sub(instruction as u32);
        data[position..position + 4].copy_from_slice(&value.to_le_bytes());
    }
    let target = instruction.wrapping_add(usize::from(read_u16(data, position)?)) as u16;
    let after = instruction
        .checked_add(opcode_bytes + 3)
        .ok_or(Error::ArithmeticOverflow("LZMS x86 instruction"))?;
    let target_index = usize::from(target);
    if (after as i32).wrapping_sub(last_targets[target_index]) <= X86_WINDOW {
        *last_x86 = after as i32;
    }
    last_targets[target_index] = after as i32;
    Ok(position + 4)
}

fn x86_filter(data: &mut [u8]) -> Result<()> {
    if data.len() <= 17 {
        return Ok(());
    }
    let mut last_targets = filled_vec(65_536, -X86_WINDOW - 1, "LZMS x86 translation table")?;
    let mut last_x86 = -X86_MAX_TRANSLATION - 1;
    let sentinel = data.len() - 8;
    let saved = data[sentinel];
    data[sentinel] = 0xe8;
    let tail = data.len() - 16;
    let mut position = 1;
    loop {
        while position < data.len() && !is_opcode(data[position]) {
            position += 1;
        }
        if position >= tail {
            break;
        }
        position = translate_x86(data, position, &mut last_x86, &mut last_targets)?;
    }
    data[sentinel] = saved;
    Ok(())
}

/// Decompresses one complete Microsoft LZMS wrapper into a newly allocated buffer.
///
/// `expected_output_size` must match the size declared by the wrapper. Input is
/// bounded by `Limits::max_file_bytes`; output is bounded by
/// `Limits::max_payload_bytes` before allocation.
pub fn decompress_lzms(
    source: &[u8],
    expected_output_size: usize,
    limits: &Limits,
) -> Result<Vec<u8>> {
    validate_wrapper_header(source, expected_output_size, limits)?;
    let mut output = Vec::new();
    output
        .try_reserve_exact(expected_output_size)
        .map_err(|_| Error::AllocationFailed("LZMS output"))?;
    output.resize(expected_output_size, 0);
    decompress_lzms_into(source, &mut output, limits)?;
    Ok(output)
}

fn validate_wrapper_header(
    source: &[u8],
    expected_output_size: usize,
    limits: &Limits,
) -> Result<usize> {
    limits.validate()?;
    let input_size =
        u64::try_from(source.len()).map_err(|_| Error::ArithmeticOverflow("LZMS input size"))?;
    if input_size > limits.max_file_bytes {
        return Err(Error::LimitExceeded {
            kind: LimitKind::FileBytes,
            limit: limits.max_file_bytes,
            actual: input_size,
        });
    }
    let output_size = u64::try_from(expected_output_size)
        .map_err(|_| Error::ArithmeticOverflow("LZMS output size"))?;
    if output_size > limits.max_payload_bytes {
        return Err(Error::LimitExceeded {
            kind: LimitKind::PayloadBytes,
            limit: limits.max_payload_bytes,
            actual: output_size,
        });
    }
    if source.len() < 28 {
        return Err(invalid("wrapper is too short"));
    }
    if source.get(..SIGNATURE.len()) != Some(&SIGNATURE) {
        return Err(invalid("invalid wrapper signature"));
    }
    if read_u64(source, 8)? != output_size {
        return Err(invalid("declared output size mismatch"));
    }
    let block_size64 = read_u64(source, 16)?;
    if block_size64 == 0 || usize::try_from(block_size64).is_err() {
        return Err(invalid("invalid block size"));
    }
    Ok(block_size64 as usize)
}

/// Decompresses one complete Microsoft LZMS wrapper into caller-owned storage.
pub fn decompress_lzms_into(source: &[u8], destination: &mut [u8], limits: &Limits) -> Result<()> {
    let block_size = validate_wrapper_header(source, destination.len(), limits)?;
    let mut input = 24_usize;
    let mut output = 0_usize;
    while output < destination.len() {
        if source.len().saturating_sub(input) < 4 {
            return Err(invalid("truncated block header"));
        }
        let compressed = read_u32(source, input)? as usize;
        input += 4;
        let remaining = destination.len() - output;
        let decoded = remaining.min(block_size);
        if compressed > decoded || compressed > source.len().saturating_sub(input) {
            return Err(invalid("invalid compressed block size"));
        }
        let block_end = input
            .checked_add(compressed)
            .ok_or(Error::ArithmeticOverflow("LZMS block end"))?;
        let output_end = output
            .checked_add(decoded)
            .ok_or(Error::ArithmeticOverflow("LZMS output block end"))?;
        let source_block = &source[input..block_end];
        let destination_block = &mut destination[output..output_end];
        if compressed == decoded {
            destination_block.copy_from_slice(source_block);
        } else {
            raw_decompress(source_block, destination_block)?;
        }
        input = block_end;
        output = output_end;
    }
    if input != source.len() {
        return Err(invalid("trailing wrapper data"));
    }
    Ok(())
}
