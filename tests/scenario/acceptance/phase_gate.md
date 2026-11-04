# QASM 인수 테스트 — Phase Gate 매핑

> 상위 문서: [ 인수 테스트]
> 본 파일은 9개 고정 QASM 케이스의 `expected_fail` 토글 시점을 한 곳에 모은다. Phase 0-T 시점의 초기 상태와 각 Phase Gate 완료 시 토글되는 케이스를 일치시켜야 한다.

---

## 1. 9개 케이스 요약

| 라벨 | qasm 파일 | 기대값 JSON | 검증 종류 (`kind`) | 통과 기대 Phase |
|------|----------|------------|-------------------|----------------|
| `bell`        | `tests/data/qasm/bell.qasm`        | `expected/bell.json`        | `counts`            | Phase 5 |
| `ghz_5`       | `tests/data/qasm/ghz_5.qasm`       | `expected/ghz_5.json`       | `counts`            | Phase 5 |
| `qft_4`       | `tests/data/qasm/qft_4.qasm`       | `expected/qft_4.json`       | `statevector`       | Phase 5 |
| `oracle_10`   | `tests/data/qasm/oracle_10.qasm`   | `expected/oracle_10.json`   | `statevector_dump`  | Phase 5 |
| `oracle_20`   | `tests/data/qasm/oracle_20.qasm`   | `expected/oracle_20.json`   | `statevector_dump`  | Phase 5 |
| `opt_cancel`  | `tests/data/qasm/opt_cancel.qasm`  | `expected/opt_cancel.json`  | `circuit_metric`    | Phase 4 |
| `noisy_bell`  | `tests/data/qasm/noisy_bell.qasm`  | `expected/noisy_bell.json`  | `fidelity`          | Phase 8 |
| `sparse_100q` | `tests/data/qasm/sparse_100q.qasm` | `expected/sparse_100q.json` | `counts_sparse`     | Phase 7 |
| `compile_swap`| `tests/data/qasm/compile_swap.qasm`| `expected/compile_swap.json`| `counts`            | Phase 8 |

`circuit_metric`(opt_cancel)은 백엔드를 실행하지 않고 `transpile(circuit, optimization_level=1)` 후 `circuit.num_ops()` 만 검증한다. 나머지 8종은 `QRegister::run()` 결과 분포/상태벡터/충실도를 검증한다.

`statevector_dump` 는 oracle 회로 시뮬레이션의 최종 statevector 를 amplitude-by-amplitude 비교한다(global phase 차이 허용 시 fidelity 가 더 적합하지만 본 검증은 *동일 시뮬레이션 경로 회귀 검출* 이 목적이라 직접 비교 채택). 두 케이스가 같은 kind 를 공유하되 **저장 방식이 다르다**:

| 케이스 | 큐비트 | 상태 수 | 저장 방식 | 파일 위치 |
|--------|--------|---------|----------|----------|
| oracle_10 | 10 | 1,024 | JSON `expected_amplitudes` 인라인 (~50KB) | `expected/oracle_10.json` 단일 파일 |
| oracle_20 | 20 | 1,048,576 | sibling `.data` 파일 메타 참조 (~60MB 텍스트 dump) | `expected/oracle_20.json` + `expected/oracle_20.data` |

스키마 규칙: `kind=statevector_dump` 의 두 저장 필드(`expected_amplitudes` · `data_file`)는 **상호 배타** — 정확히 하나만 존재해야 한다. 검증 본문이 JSON 로드 후 어느 필드가 있는지 분기. oracle_20 처럼 num_states ≥ 2^14 수준이면 sibling `.data` 가 권장(인라인 시 git history / IDE 부담). qelib1 25 게이트 자극 + barrier 사용은 두 회로 모두 정합.

## 2. 누적 통과 타임라인 (재배열 반영)

| Phase 완료 | `expected_fail=false` 로 토글되는 케이스 | 누적 통과 | 비고 |
|-----------|----------------------------------------|---------|------|
| Phase 0-T | — | **0/9** | 9개 모두 `expected_fail=true`. `parse_file()` → `NotImplementedError` → `NOT_IMPLEMENTED` → exit 0 |
| Phase 1 | — | 0/9 | 자체 테스트 프레임워크만 추가. acceptance runner 동작 변화 없음 |
| Phase 2 | — | 0/9 | Circuit IR 완성. 파서 미완성이라 `parse_file()` 여전히 throw |
| Phase 3 | — | 0/9 | OpenQASM v2 파서 완성. `parse_file()` 통과하지만 `transpile()`·`run()` 미완성으로 여전히 throw |
| **Phase 4** | `opt_cancel` | **1/9** | PassManager + CancelInversePass. 백엔드 없이 `transpile()` 검증 |
| **Phase 5** | `bell`, `ghz_5`, `qft_4`, `oracle_10`, `oracle_20` | **6/9** | FullState CPU + QRegister Batch API. Interactive 제외. oracle_10/20 은 qelib1 직접 표현 25 게이트 전부 자극 |
| Phase 6 | — | 6/9 | FullState GPU. 기존 6종이 GPU 백엔드에서도 통과 검증. `expected_fail` 토글 없음 (CPU 백엔드 기준으로 이미 통과) |
| **Phase 7** | `sparse_100q` | **7/9** | SparseState. 노이즈 미지원 |
| **Phase 8** | `noisy_bell`, `compile_swap` | **9/9 ✅** | NoiseModel + DeviceModel + Interactive + F-CC |
| Phase 9 | — | 9/9 ✅ | TN 기본. **TN 백엔드 인수 실행은 4/9** — `--sim=tn` 모드 |
| Phase 10 | — | 9/9 ✅ | TN 노이즈. TN vs FullState CPU parity ±0.01 |
| Phase 11 | — | 9/9 ✅ | Python 바인딩. Python acceptance runner 9/9 통과 |

## 3. 토글 절차 (각 Phase Gate 진입 시)

1. 해당 Phase의 통과 기대 케이스를 `tests/scenario/acceptance/run_acceptance.cpp` `default_phase0t_cases()` 에서 `expected_fail=true` → `false` 로 변경.
2. 빌드 후 `./build/tests/qplayer_acceptance` 실행 → 해당 케이스가 `PASS` 로 집계되는지 확인 (`NOT_IMPLEMENTED` 또는 `FAIL` 이면 즉시 회귀).
3. `ctest --test-dir build -R qplayer_acceptance` 0 exit 확인.
4. 본 파일(`tests/scenario/acceptance`) 2절 표의 해당 행에 ✅ 표기 추가.
5. `` 의 해당 Phase 항목 ✅ 완료 + QASM 누적 카운트 갱신.

기대값(JSON) 자체를 갱신해야 할 경우 반드시 ADR 선행 작성(Golden test 규칙 준용 · ADR 카테고리 03 또는 09).

## 3-A. TN 백엔드 모드 (`--sim=tn`, Phase 9~)

기본 실행(`./build/tests/qplayer_acceptance`)은 9개 케이스를 FullState 기준 경로로 돌린다. Phase 9 부터 **`--sim=tn`** 이 TN 백엔드가 감당할 수 있는 부분집합만 `SimType::TensorNetwork` 로 다시 돌린다(CTest 엔트리 `qplayer_acceptance_tn`). 누적 `9/9` 요약을 흔들지 않도록 **별도 호출**로 분리했다.

각 row 의 `phase_gate` 라벨은 **회로가 추가된 Phase 가 아니라 TN 에서 돌게 만든 Phase** 다 — 그래야 누적 카운트가 TN 진척으로 읽힌다.

| 라벨 | kind | TN 실행 | gate | 근거 |
|---|---|:---:|---|---|
| `bell` · `ghz_5` | counts | ✅ | Phase 9 | MPS 경로 1Q·2Q 게이트만 사용 |
| `qft_4` | statevector | ✅ | Phase 9 | n ≤ `MAX_DENSE_EXPORT_QUBITS` 라 `get_statevector()` 가능 |
| `opt_cancel` | circuit_metric | ✅ | Phase 9 | `transpile()` 만 검증 — 백엔드 무관 |
| `noisy_bell` | fidelity | ✅ | **Phase 10** | TN 노이즈 도입으로 `execute_trajectories()` 가 그대로 감당 |
| `compile_swap` | counts | ✅ | **Phase 10-A** | 검증 대상("삽입된 SWAP 후에도 논리 결과 유지")은 백엔드 무관이고, 라우팅된 물리 회로가 MPS 에서 돈다 — SWAP 은 2Q 이고 커플링 맵이 MPS 가 선호하는 선형 체인이다 |
| `oracle_10` · `oracle_20` | statevector_dump | ✗ | — | **`ccx` 사용**. MPS 경로가 1Q·2Q 적용만 구현하므로 TN 백엔드가 3큐빗 이상 게이트를 거부한다(`tn_fusion.cpp`). 3Q 분해 패스를 백엔드 앞단에 두면 편입 가능 |
| `sparse_100q` | counts_sparse | ✗ | — | kind 가 SparseState 를 고정 — 백엔드 선택 사항이 아니다 |

> **이력**:
> - 2026-08-25: `` Phase 9 절이 당초 TN 통과 회로를 "의미 있는 6개"(oracle 2건 포함)로 적었으나, `--sim=tn` 실행에서 두 회로가 3큐빗 게이트 거부로 FAIL 하는 것이 확인돼 **4개**로 정정했다.
> - 2026-08-26: `noisy_bell`·`compile_swap` 편입으로 **4 → 6**. `compile_swap` 은 종전 "디바이스 파이프라인이라 TN 축과 무관" 으로 제외돼 있었는데, 그 판단은 *왜 제외하는가* 가 아니라 *아직 검증하지 않았다* 였다 — 실제로 돌려 보니 통과한다.

## 4. exit_code 정책 요약

| 상황 | exit_code |
|------|-----------|
| 모든 케이스가 `PASS` 또는 (`expected_fail=true` ∧ `NOT_IMPLEMENTED`) | **0** |
| 하나라도 `FAIL` / `UNEXPECTED_PASS` / `UNEXPECTED_FAIL` | 1 |
| 하나라도 `expected_fail=false` 인 케이스가 `NOT_IMPLEMENTED` | 1 |

CTest 는 본 runner 의 종료 코드를 그대로 사용한다 — regex matching 미사용.

## 5. 검증 종류별 기대값 스키마

| `kind` | 필수 필드 | 비교 로직 활성 Phase |
|--------|----------|---------------------|
| `counts` | `nshots`, `tolerance`, `expected_counts` (확률값 0~1) | Phase 5 |
| `statevector` | `nshots`(=1), `tolerance_l2`, `expected_statevector_uniform_amplitude`, `expected_state_dimension` | Phase 5 |
| `statevector_dump` | `num_qubits`, `num_states`, `tolerance_l2`, `tolerance_per_amplitude` + 상호 배타 1종 — ① `expected_amplitudes` (`[re, im]` 쌍의 길이 `num_states` 배열, 인덱스 = 비트스트링 unsigned 정수 MSB→LSB, oracle_10) 또는 ② `data_file` (sibling `.data` 텍스트 dump 파일명, `[P=<%.6f>] [<%+.6f>, <%+.6f>] |<bitstring>>` 라인 `num_states` 행, oracle_20) | Phase 5 |
| `circuit_metric` | `metric`(=`"num_ops_after_transpile"`), `optimization_level`, `expected_value` | Phase 4 |
| `fidelity` | `nshots`, `tolerance`, `noise_model`, `expected_fidelity` | Phase 8 |
| `counts_sparse` | `nshots`, `tolerance`, `expected_nonzero_amplitude_count`, `expected_counts` | Phase 7 |

`statevector_dump` 비교 본문 (Phase 5 패치 시 구현):
1. JSON 로드 후 저장 방식 분기:
   - `expected_amplitudes` 존재 → 배열 길이 == `num_states` 검사 → `[re, im]` 쌍 → `std::vector<std::complex<double>>` 변환.
   - `data_file` 존재 → sibling `.data` 파일 로드 → 헤더 1줄(`============== dump <N> states =============`) skip → 본문 `num_states` 행을 `\[P=([0-9.+-]+)\] \[([+-][0-9.]+), ([+-][0-9.]+)\] \|([01]+)>` 정규식 파싱 → `re + i·im` 벡터 구성. line count != `num_states` 또는 비트스트링 길이 != `num_qubits` 시 `Status::FAIL`.
   - 두 필드 모두 존재하거나 모두 부재 → `Status::FAIL` (schema 위반).
2. `QRegister::get_statevector()` 결과(`std::vector<std::complex<double>>`) 와 amplitude-by-amplitude 비교 — `|actual[i] - expected[i]| > tolerance_per_amplitude` 첫 위반 인덱스 보고 + `Status::FAIL`.
3. 전체 L2 norm 차이 `sqrt(Σ |actual[i] - expected[i]|²)` 가 `tolerance_l2` 초과 시 누적 위반 보고.
4. (verbose mode) `.data` 경로의 경우 P invariant `|P − (re²+im²)| < 5e-7` 검사 — 외부 제공 dump 의 자체 무결성 안전망.

JSON 비교 본문은 `run_acceptance.cpp` 안에 Phase 4~8 패치로 점진 추가된다. Phase 0-T 시점에는 모든 케이스가 `parse_file()` 단계에서 `NotImplementedError` 로 종료되므로 비교 본문 진입이 없다 — 본 표는 후속 Phase의 패치 가이드 역할을 한다.
