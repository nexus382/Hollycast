# Repeated artwork scan measurements

Repeated browsing of a generated library without embedded artwork uses less CPU after the absence-cache fix. Ten full passes over 1,000 games fell from 2,355.917 ms to 3.532 ms process CPU, a 99.8501% reduction. This measures the native artwork subsystem through its production queue and workers.

The [sample evidence](../../tools/artwork-benchmark/sample-results.json) contains five interleaved timing pairs, phase aggregates, and a separate counted pair. The [rerun instructions](../../tools/artwork-benchmark/README.md) build the same external workload against both sources.

## Repeat browsing after the first scan

Timing medians use five pairs without the I/O interposer. Logical I/O uses one separate pair with counter self-tests. Its timings are excluded.

| Metric for ten passes | Before | After |
| --- | ---: | ---: |
| Process CPU | 2,355.917 ms | 3.532 ms |
| Elapsed time | 1,454.368584 ms | 3.601250 ms |
| CPU range | 2,298.700 to 2,455.325 ms | 2.961 to 4.402 ms |
| Elapsed range | 1,349.709208 to 1,639.744333 ms | 2.961458 to 4.805875 ms |
| Logical stdio reads | 62,100,000 bytes | 0 bytes |
| Logical database writes | 92,500,500 bytes | 0 bytes |
| macOS attributed disk writes, median | 93,184,000 bytes | 0 bytes |
| macOS attributed disk reads, median | 0 bytes | 0 bytes |
| CUE opens, actual disc scans | 10,000 | 0 |
| BIN opens | 20,000 | 0 |
| Database write calls | 250 | 0 |

Elapsed time fell by 99.7524%. Every measured process reported zero validation errors and exactly 1,000 persisted records. Each batch checked product ID, title, region, empty artwork, idle completion, and no online scrape.

The logical read count matches 10,000 scans of one 66-byte CUE and three 2,048-byte sectors. The original code also rewrote its 370,002-byte database after each 40-game batch. Those 250 writes account for 92,500,500 bytes. The fixed repeat phase reuses completed absence results and skips this work.

Separate profiling found `OpenDisc`, CUE parsing, `fopen`, and `fread` in scanner workers, and `saveDatabase` with JSON serialization in the outer worker. Those concurrent stacks identify repeated parsing and serialization as the costs removed here. Sample counts are not exclusive CPU percentages.

## Other phases and costs

All figures below are medians from the same five uninstrumented timing pairs.

| Phase over 1,000 games | Before CPU | After CPU | Before elapsed | After elapsed |
| --- | ---: | ---: | ---: | ---: |
| First scan | 232.975 ms | 233.147 ms | 166.898541 ms | 126.354416 ms |
| Reconstruct service and reuse persisted database | 251.748 ms | 4.182 ms | 158.264417 ms | 4.306625 ms |
| Explicit refresh and rescan | 238.131 ms | 261.292 ms | 134.395833 ms | 187.556084 ms |

First-scan CPU and elapsed ranges overlap. These runs establish no first-scan improvement. Explicit refresh remains a full rescan and was slower after the fix in these runs. The persisted database grew from 370,002 to 530,002 bytes, an extra 160,000 bytes for source paths and outcome state. The persistence phase reconstructs the artwork service in one process. It does not measure a full application relaunch.

## Conditions and limits

The baseline is `c517c36618029fb65d495d0333211b469b147c7a`. Both sources used the same C++ workload and arm64 Ninja build with `RelWithDebInfo`, `-O2 -g -DNDEBUG`, and matching feature flags. The host ran macOS 26.6 with 10 CPU cores and 32 GiB RAM. Desktop, browser, and IDE processes remained active. The recorded host load was high. Alternating pair order gave both sides exposure to that load.

The fixture contains 1,000 distinct valid Dreamcast CUE tracks without embedded artwork. Each BIN is a sparse 700 MiB file. Generating the fixture precedes timing. Online downloading is disabled. The real offline scraper uses four parallel workers, so process CPU can exceed elapsed time. Both versions validate metadata and join workers inside each timed phase. The remaining repeat time includes queue, cache lookup, and validation overhead.

These are warm OS-cache runs. Median attributed physical reads were zero on both sides, so the logical read reduction does not establish an SSD-read reduction. macOS attributed write bytes include allocation rounding. The database writer uses `fflush` and `fclose` without `fsync`. These counts do not measure durable SSD completion or NAND wear.

The workload does not measure rendering, gameplay FPS, compressed CHD or CDI tracks, remote storage, mixed artwork libraries, or online fallback. The measured benefit applies to reuse of a completed no-artwork result.
