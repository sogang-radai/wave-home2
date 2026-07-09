# Sleep pipeline (wave-server)

Live sleep ingestion is handled by `SleepManager` (`src/wave-server/service/sleep/sleep_manager.cpp`).

## Radar selection

Only radars with `settings.sleep: true` in `bin/device/device_list.json` are loaded (e.g. bedroom radar). Desk radars without the flag are ignored.

## Model

- Default model: `sleep0-1` (`bin/config.json` → `sleep_model_path`)
- Outputs: `absent` / `awake` / `asleep`, toss `calm` / `slight` / `moderate`, `toss_index`
- Stage (light/deep/rem), HR/BR, and snore are **synthesized or stubbed** in Phase F — not from the LSTM model

## Flow

1. Radar point cloud (port **29172**) → `SleepPipeline`
2. 1s snapshots → 1m `sleep_stat` (coverage ≥ 0.3)
3. 30×1m → 30m `sleep_stat` + stage synthesis + vitals/snore placeholders + agent `summary_text`
4. Session FSM → `sleep_session` on wake confirmed (`stage_totals`, avg HR/BR/snore)
5. Agent jobs → `sleep_report` (daily/weekly) + optional embeddings

## Session rules

- **Onset**: 5 minutes with ≥4 minutes `asleep_r ≥ 0.85`
- **Wake confirmed**: 5 min `awake+absent ≥ 0.7` or 3 min `absent ≥ 0.8`
- **Micro-arousal**: awake &lt; 3 min does not split session

## Phase F (scaffolding)

| Component | File | Notes |
|-----------|------|-------|
| Stage synthesis | `service/sleep/sleep_stage_synth.cpp` | 90-min cycle heuristic on 30m windows |
| Vitals | `service/sleep/sleep_vitals.cpp` | PC centroid target picker; HR/BR stub (IQ port **29171** when ready) |
| Snore | `estimateSnoreRatio()` | Placeholder from toss + env |
| Embeddings | `service/sleep/sleep_vec_store.cpp` | `vec_sleep_*` if sqlite-vec enabled, else `sleep_*_embedding` blob tables (migration v11) |

## Future

- Real IQ DSP for HR/BR (`hr_confidence > 0`)
- WaveStation mic for snore
- sqlite-vec virtual tables (`vec_sleep_stat`, `vec_sleep_report`)
