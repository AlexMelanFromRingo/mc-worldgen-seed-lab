// orig_bench <файл> <потоков> [--paper] [--structure]
// Формат файла как у crack-nether-bedrock: "x y z [bedrock|other]" (без типа = bedrock, как в GUI оригинала).
use bedrock_cracker::raw_data::block::Block;
use bedrock_cracker::raw_data::block_type::BlockType;
use bedrock_cracker::raw_data::modes::{BedrockGeneration, OutputMode};
use bedrock_cracker::{estimate_result_amount, search_bedrock_pattern, CrackProgress};
use std::io::BufRead;
use std::time::Instant;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let path = args.get(1).expect("файл").clone();
    let threads: u64 = args.get(2).expect("потоков").parse().unwrap();
    let paper = args.iter().any(|a| a == "--paper");
    let structure = args.iter().any(|a| a == "--structure");
    // --max-events N: остановиться после N событий прогресса (каждое = 2^25 префиксов на поток) — замер пропускной способности без полного прохода
    let max_events: u64 = args.iter().position(|a| a == "--max-events").map(|i| args[i + 1].parse().unwrap()).unwrap_or(u64::MAX);
    let mut events: u64 = 0;
    let mut blocks = Vec::new();
    for l in std::io::BufReader::new(std::fs::File::open(path).unwrap()).lines() {
        let l = l.unwrap();
        let l = l.split('#').next().unwrap().to_string();
        let p: Vec<&str> = l.split_whitespace().collect();
        if p.len() < 3 { continue; }
        let y: i32 = p[1].parse().unwrap();
        if !((1..=4).contains(&y) || (123..=126).contains(&y)) { continue; }
        let bt = if p.len() >= 4 && p[3].eq_ignore_ascii_case("other") { BlockType::OTHER } else { BlockType::BEDROCK };
        blocks.push(Block::new(p[0].parse().unwrap(), y, p[2].parse().unwrap(), bt));
    }
    let mode = if paper { BedrockGeneration::Paper1_18 } else { BedrockGeneration::Normal };
    let out = if structure { OutputMode::StructureSeed } else { OutputMode::WorldSeed };
    println!("blocks: {}  estimate_result_amount: {}", blocks.len(), estimate_result_amount(&blocks));
    let (tx, rx) = std::sync::mpsc::channel();
    let t0 = Instant::now();
    search_bedrock_pattern(&blocks, threads, mode, out, tx);
    let mut total: u64 = 0;
    let target: u64 = (1u64 << 36) << 12;
    let mut seeds = vec![];
    loop {
        match rx.recv() {
            Ok(CrackProgress::Seed(s)) => {
                println!("seed {} at {:.2}s", s as i64, t0.elapsed().as_secs_f64());
                seeds.push(s as i64);
            }
            Ok(CrackProgress::Progress(p)) => {
                total += p;
                events += 1;
                if events >= max_events {
                    let dt = t0.elapsed().as_secs_f64();
                    let prefixes = (total >> 12) as f64;
                    println!("partial: {} событий, {:.3e} префиксов за {:.2}s = {:.3e} префиксов/с (wall, {} потоков)", events, prefixes, dt, prefixes / dt, threads);
                    break;
                }
                if total >= target { break; }
            }
            Err(_) => break,
        }
    }
    println!("done in {:.2}s, {} seeds, threads={}", t0.elapsed().as_secs_f64(), seeds.len(), threads);
}
