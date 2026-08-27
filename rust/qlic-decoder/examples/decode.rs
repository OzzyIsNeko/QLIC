use std::env;
use std::error::Error;
use std::fs::{self, File};
use std::io::{BufWriter, Write};

use qlic_decoder::{Limits, decode_rgba};

fn main() -> Result<(), Box<dyn Error>> {
    let mut arguments = env::args_os();
    let program = arguments.next().unwrap_or_default();
    let input = arguments.next();
    let output = arguments.next();
    let (Some(input), Some(output)) = (input, output) else {
        eprintln!(
            "usage: {} input.qlic output.pam",
            std::path::Path::new(&program)
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
        );
        std::process::exit(2);
    };
    if arguments.next().is_some() {
        eprintln!(
            "usage: {} input.qlic output.pam",
            std::path::Path::new(&program)
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
        );
        std::process::exit(2);
    }

    let encoded = fs::read(input)?;
    let image = decode_rgba(&encoded, &Limits::default())?;
    let file = File::create(output)?;
    let mut writer = BufWriter::new(file);
    write!(
        writer,
        "P7\nWIDTH {}\nHEIGHT {}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
        image.width, image.height
    )?;
    writer.write_all(&image.rgba)?;
    writer.flush()?;
    Ok(())
}
