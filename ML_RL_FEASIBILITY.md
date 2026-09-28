# RL/ML for Haven — what's real, what's buildable now, what isn't

Written 2026-09-25, in response to being asked to look into RL/ML features
that could help figure out what's causing discomfort. Grounded in real
precedent (cited), not speculation. Current state, confirmed exhaustively
this session: zero ML/RL code exists anywhere in any Haven repo today. This
is what could exist, in three honest tiers.

## First, a distinction that decides everything below

- **Tinnitus** (a phantom ringing/buzzing/hissing) is generated internally
  by the auditory nerve. The large majority of cases ("subjective tinnitus")
  have **no external acoustic signature** — there is nothing in a microphone
  recording that corresponds to it. No ML model, no amount of data, can
  detect tinnitus's "cause" from ambient audio, because there usually isn't
  one to detect. The only audio-based approach that works is what Haven
  already has: ask the person to match the *perceived* pitch/loudness
  themselves (the existing 2AFC pitch-match flow). That's not a gap to fill,
  it's already the right approach.
- **Hyperacusis** (pain/discomfort from ordinary external sounds) is
  different — the sound is real and recordable. This is the one where
  "figure out what's causing the pain" is a legitimate ML/DSP problem:
  correlating what's in the environment with when the user reports
  discomfort.

Everything below is about the hyperacusis/external-sound case.

## Tier 1 — buildable now, no dataset, real precedent

**Preference-learning ("dueling bandit") tuner for the filter settings.**
Widex ships almost exactly this in a commercial hearing aid product
(SoundSense Learn): instead of a person manually dragging sliders across a
huge parameter space, the device presents repeated A/B comparisons ("which
sounds better, this or this?") and a bandit algorithm converges on the
person's preferred setting in about a dozen comparisons, out of a space of
2,000+ possible combinations. [Widex's own explanation](https://www.widexpro.com/en-us/widex-technology/artificial-intelligence/sound-sense-learn/).
Academic versions of the same idea exist for hearing aids specifically,
including dueling-bandit and human-in-the-loop RL approaches to personalize
compression and comfort targets ([MDPI review, 2024](https://www.mdpi.com/1424-8220/24/5/1546);
[arXiv:2007.00192](https://arxiv.org/abs/2007.00192), human-in-loop deep RL
for hearing-aid compression).

Haven's existing `MULTI_FILTER` parameter space (`f0`, `Q`, `atten_db` per
band, up to 5 bands) is exactly the kind of space this technique targets.
This needs **no pre-existing dataset** — it learns online, per person, from
their own A/B choices, starting the moment someone uses it. This is the
single most realistic, differentiated thing to build next, and it directly
answers "help me find the setting that actually feels right" without ever
touching the "detect the cause" question dishonestly.

**A bandit-paced tolerance plan — built 2026-09-27, as an adaptive rule,
not a bandit.** The existing tolerance-building feature
(`constants/tolerance.ts`) steps `attenDb` down by a fixed amount, always
on explicit user tap; this update adapts the *wait* before the next step
becomes available, based on recent comfort check-in responses.

Two things were wrong in the paragraph above before this got built, both
worth recording rather than quietly fixing:

1. **"the comfort check-in responses already being collected" was false.**
   Checked before building on it: the comfort check-in only ever persisted
   *when* it last fired (`ComfortStore`), never *what* the person answered.
   The direction was used once to nudge `attenDb` and then discarded. Fixed
   first — `ComfortHistoryStore` now persists a real rolling history
   (`haven-app`) — before anything could adapt on it.
2. **"a small contextual bandit" turned out to be the wrong tool once the
   real data density was considered, not just a simplification.** A
   comfort response happens at most about once a day; tolerance steps are
   roughly a week apart. That's at most one or two data points between
   decisions — nowhere near enough for a meaningful explore/exploit
   tradeoff the way the depth/width tuner's dozen-comparisons-in-one-
   sitting setup has. Built as a plain, documented rule instead
   (`utils/tolerancePacing.ts`, haven-app): two "too strong" responses
   since the last step double the wait; two comfortable/less-softening
   responses halve it (with a floor and ceiling); anything mixed or absent
   leaves it unchanged. Calling that a "bandit" would have been the
   dishonest kind of overclaiming this document exists to avoid.

25 new tests across the new history store, the pacing function, and both
hooks it touches — including that the countdown UI (`TolerancePlanCard`)
now reflects the actual adapted wait, not the fixed constant, which would
have silently drifted out of sync with `dueForStep` otherwise.

## Tier 2 — buildable, needs more dev work, modest data

**Continuous (not offline-batch) version of the existing PSD tool.** The
`ml/analyze.py` heuristic that already exists (Welch PSD, dominant-peak
half-power bandwidth) currently runs on a single recorded .wav file. Running
the same math over a rolling window of live mic audio — on the phone, or
on-device — turns "what was the troublesome frequency in this recording"
into "what's the troublesome frequency right now." This is still fixed DSP,
not a learned model, and it's the honest, buildable version of "figure out
what in the environment is bothering me."

**On-device sound-event classification.** Nordic officially supports Edge
Impulse's tinyML workflow on the nRF5340 — train a small sound classifier
(traffic, machinery, alarm, speech, quiet) and run it directly on the
Cortex-M33, no cloud, no phone required for inference
([Nordic/Edge Impulse announcement](https://www.edgeimpulse.com/blog/nordic-embedded-machine-learning/)).
This needs real recordings to train on — hours, not the huge corpora deep
learning usually implies, and Edge Impulse's workflow is built for exactly
this scale — but it's a real dataset requirement, unlike Tier 1. Correlating
"this kind of sound was present" with "the user reported discomfort around
this time" is what would let Haven eventually say something like "traffic
noise tends to bother you more than machinery," which is a genuinely useful,
honest claim distinct from "detecting the cause of your tinnitus."

## Tier 3 — don't build, don't claim

- **Anything framed as detecting or diagnosing the source of tinnitus from
  ambient audio.** Not achievable for the majority of cases — see above.
- **Full deep RL from scratch**, as opposed to a bandit. The academic work
  in this space (e.g. the arXiv paper above) still relies on simulated
  patient models to pretrain before ever touching a real user, because one
  person's real interactions are far too few data points to train a deep RL
  policy from nothing. A bandit is the right tool at this data scale; deep
  RL is not, at least not for a v1.

## Recommendation

Start with the Tier 1 dueling-bandit tuner over the existing `MULTI_FILTER`
parameters. It's real, it's precedented in a shipping commercial product,
it needs zero training data, and it replaces "the user guesses at sliders"
with something that's honestly describable as personalization. Tier 2's
on-device classifier is a real second step once there's a reason to collect
the training recordings. Tier 3 stays off the roadmap and out of any
marketing copy.

## Update 2026-09-27: built, and two more real options found

**The Tier 1 bandit tuner is built** — `haven-app` PR #10, "Add a
preference-guided tuner for softening depth and width." Two phases (depth,
then width), same algorithm class described above, 15 new tests including
one that caught a real off-by-one convergence bug before it shipped (an
earlier version of the search could stop one comparison short and silently
lock in the wrong candidate at the edge of the range).

**A "frontier AI model" feature, prototyped**: `haven-app` PR #11 — a
deterministic trend-insights layer over the existing LDL/match history
(real stats, no AI at all, useful on its own), plus a tested-but-not-live
LLM rewrite step that turns those facts into a plain-language paragraph.
The actual safety property that makes this honestly buildable in a
health-adjacent app: every number the model outputs is checked against the
real source data, and any fabricated number (an invented percentage, an
untested frequency) discards the response and falls back to the
deterministic text. **Not wired to a live API key anywhere** — a client app
must never embed a provider key in its bundle, so this needs a small
backend first (holds the key, the app calls that instead), which is a real,
separate cost/ops decision, not something to add unilaterally. Full
writeup: `haven-app/docs/llm-summary.md`.

**Two more real Tier 2 options found, both with concrete precedent:**

- **YAMNet for on-device/phone sound-event classification** — a real,
  publicly available pretrained model (521 sound classes, trained on
  AudioSet) with an existing TFLite conversion and React Native bindings
  ([TensorFlow's own writeup](https://blog.tensorflow.org/2021/09/easy-machine-learning-for-on-device-audio.html);
  [YAMNet→TFLite conversion](https://medium.com/@antonyharfield/converting-the-yamnet-audio-detection-model-for-tensorflow-lite-inference-43d049bd357c)).
  The real advantage over the Tier 2 idea in this doc's first version:
  **no custom training data needed at all** — it's pretrained on a broad
  general-sound corpus, so tagging ambient context (traffic, alarm, speech,
  machinery, quiet) could start immediately instead of waiting on a
  recordings-collection step. The real cost: React Native's TFLite tooling
  needs a custom dev client, not Expo Go or the web build — same
  constraint already blocking `react-native-ble-plx` bring-up, so this
  would ride along with that work rather than needing its own new
  infrastructure (the dev-client prep for that is now done — see
  `haven-app/docs/dev-client-build.md`). Not prototyped yet.

  **Update 2026-09-27, two more real data points, one of them a risk
  flag**: YAMNet's own real latency number — about 100ms to process a 2s
  audio window via a CPU delegate with 2 threads on Android
  ([search-sourced benchmark](https://github.com/farmaker47/Yamnet_classification_project)) —
  comfortably fits a non-real-time context-tagging use case (well under the
  window length itself), consistent with keeping this off the hear-through
  path as already scoped. Model file size wasn't confirmed to a specific
  number this pass — don't cite one without checking the actual exported
  file. The risk flag: `react-native-fast-tflite` (the binding this would
  need) has a **documented GitHub issue**
  ([mrousavy/react-native-fast-tflite#133](https://github.com/mrousavy/react-native-fast-tflite/issues/133))
  reporting a build failure specifically with the New Architecture enabled
  on iOS, at v1.6.0 / RN 0.77.1. **Follow-up, same day: checked the issue's
  actual resolution rather than leaving it as an open flag.** It's closed,
  and the real root cause was narrower than "New Architecture
  incompatibility" — v1.6.0's npm release shipped without a required `spec`
  folder needed for New Arch codegen (a packaging oversight, not a design
  incompatibility); it didn't happen on v1.5.0 or with the New Architecture
  disabled. This meaningfully de-risks the library for this project: the
  New Architecture support itself isn't the problem, one specific release's
  packaging was. Still worth confirming the version actually installed
  isn't the broken v1.6.0 specifically before relying on it, but this is no
  longer an open question the way it was left last update.

  **Checked whether YAMNet could be prototyped on the web build instead of
  waiting on the dev-client build — it can't, at least not the easy way.**
  The idea: `@tensorflow/tfjs` (the plain browser package, unrelated to
  `tfjs-react-native`, which needs `expo-gl` and doesn't work in a managed
  web build anyway) runs completely normally in Expo's web target, since
  that's just an ordinary web bundle. If a ready-made TFJS YAMNet existed,
  it could've been prototyped and tested today, the same way the LLM-relay
  and rolling-analysis work was, sidestepping the dev-client wait entirely.
  Checked directly rather than assumed: TF Hub's own URL convention for a
  ready TFJS export (`?tfjs-format=compressed` appended to a model's TF Hub
  URL) returns a real 404 for YAMNet — confirmed with a direct fetch, not
  inferred from documentation. No official TFJS-converted YAMNet exists via
  that path. Building one would mean running `tensorflowjs_converter`
  against the raw TF Hub SavedModel myself, a real undertaking (needs the
  Python TF toolchain, produces an artifact with no ground truth to
  validate it against without separately obtained reference audio) — not
  something to attempt speculatively without a clearer reason to invest in
  it. YAMNet stays exactly where the last update left it: real, cited,
  waiting on the dev-client build, not on this web shortcut.
- **RNNoise for mic noise suppression** — a real, widely-shipped open-source
  library (used in Mumble, OBS) that pairs classic DSP with a small
  recurrent network to suppress background/wind noise in real time
  ([project background](https://hobo.house/2024/03/01/easy-noise-suppression-with-rnnoise/)).
  **Update 2026-09-27, after actually running the numbers: not a fit for
  either of Haven's two obvious on-device placements, for two different
  concrete reasons** — see below. Reframed to where it does fit.

Neither is built yet. Both are real and cited.

## Update 2026-09-27: RNNoise feasibility, done properly with real numbers

Last entry treated "there's an STM32 port" as enough to call this a plausible
on-device candidate. That was too quick — actually placing it in Haven's real
architecture rules out both obvious spots, for two different, specific
reasons found by pulling real numbers rather than assuming a port on similar
silicon means it fits everywhere.

**RNNoise's real cost**: 96k parameters (~88k weights), ~40 MFLOPS total for
real-time 48 kHz operation (17.5 MFLOPS DNN + 7.5 MFLOPS FFT/IFFT + 10 MFLOPS
pitch search), per Jean-Marc Valin's own numbers
([W3C talk](https://www.w3.org/2020/Talks/mlws/jmv_rnnoise.pdf)). The STM32
port that prompted this ran on an STM32L476 (Cortex-M4, 80 MHz, 96 KB RAM),
achieving real-time
([Jianjia Ma's writeup](https://medium.com/analytics-vidhya/real-time-rnn-speech-noise-suppression-on-a-microcontroller-stm32-e17d8c3eac57)).

**Placement 1 — the nRF5340 app core.** On paper this looks fine: 128 MHz
(1.6x the STM32 port's clock) and 512 KB RAM (5x the STM32 port's, where
RNNoise already ran real-time) comfortably cover the algorithm's raw
MFLOPS/memory footprint. **But this is the wrong place regardless of whether
it fits** — putting any real-time audio DSP on the nRF5340 app core is
exactly the risk this project already decided against when it chose to keep
the ADAU1860 rather than move filtering to the microcontroller (0.7-9 ms
added hear-through latency, comb filtering, ~5.6 mA extra draw — see
`HARDWARE_COST_ALTERNATIVES.md` in `haven-dev-board-kicad`). Fitting in terms
of cycles and RAM doesn't matter if using them here reopens a latency
decision that's already settled. Don't put it here.

**Placement 2 — the ADAU1860's FastDSP core.** This is the chip that's
actually supposed to own real-time audio, so it's the placement worth
checking properly. Its real, cited number: FastDSP runs at 24.576 MHz and
budgets 32 instructions per sample at a 192 kHz sample rate (the same rate
this project's own `tools/dsp` work already recommends) — a datasheet figure
sized for the always-on, per-sample arithmetic a biquad filter needs, not
for general-purpose branching or matrix-vector work. RNNoise's DNN component
runs per 10ms *frame* (not per sample) and needs a recurrent network with
~88k weights evaluated each frame — categorically more (and a different
shape of) computation than a 32-instruction-per-sample fixed-filter budget
is built to hold, and there's no indication FastDSP has the program memory
or instruction set for a matrix-multiply-heavy recurrent net at all. This
reads as infeasible on the numbers available, not just "unconfirmed" —
though I haven't seen FastDSP's full instruction set or program RAM size
(the ADI datasheet PDF has twice failed to fetch directly from this
session), so this should be a quick confirmation with ADI or in the full
datasheet before being treated as fully certain.

**A third placement was proposed here, then tested, then withdrawn.** The
first version of this update suggested "the phone, not the device" — denoise
a recording before `ml/analyze.py`'s PSD-peak detection runs on it, since a
phone CPU runs RNNoise trivially and it's off the real-time path. That
placement sidesteps the two problems above, but it turns out to have a
third, more basic problem: **RNNoise is a *speech* denoiser, and
`analyze.py`'s whole job is finding a narrowband, non-speech tone** (a
machine whine, an alarm, feedback) — exactly the kind of stationary,
non-speech content a speech-trained model is built to suppress, not
preserve. Tested this directly rather than assuming it either way:
`ml/experiments/rnnoise_feasibility_check.py` runs a loud pure 1 kHz tone
(favorable SNR — the best case for it to survive), a crude speech-like
harmonic+AM proxy, and a frequency-modulated "warbling alarm" proxy, each
plus noise, through RNNoise. Real, reproducible result: **0.1%, 0.9%, and
36.6% of the input energy survived, respectively** — RNNoise suppressed
almost all of even the loud, high-SNR pure tone, and most of the alarm
proxy too. (Also found and worked around a real bug in the installed
`pyrnnoise==0.4.5` package along the way: its own `denoise_wav()`
convenience method raises `AttributeError` — `Reader.rate` doesn't exist,
the real attribute is `sample_rate` — so the experiment calls the
lower-level, correctly-implemented `denoise_chunk()` directly instead.)

**Caveat, stated plainly**: these are synthetic proxies, not real recorded
speech or real recorded alarms — RNNoise's actual trained model responds to
real speech statistics these signals only loosely approximate, so this is a
real, reproducible red flag from direct testing, not a mathematical proof
it can never apply here. But it's enough to withdraw the "denoise before
`analyze.py`" recommendation until tested against real recordings, and it
means RNNoise's plausible use for Haven, if any, narrows to something
speech-shaped specifically — own-voice pickup or conversational
hear-through enhancement — not general environmental-tone or alarm cleanup.
Nothing here recommends building that; it's a narrower, unconfirmed
possibility, not a next step.

**RNNoise is currently Tier 3 for this project as a result** — not because
it doesn't work (it clearly does, at suppressing non-speech content), but
because every placement checked either reopens a settled architecture
decision (nRF5340), doesn't have the right kind of compute (ADAU1860
FastDSP), or actively works against the specific problem Haven needs solved
(suppressing the tones the project is trying to find, not remove).

**The more general lesson from this whole pass**: "a similar chip ran this"
or "this library exists" is a start, not a placement decision or a fitness
decision. Two different checks caught two different real problems here —
whether the *target hardware's* specific budget matches the workload's
shape (FastDSP), and whether the *tool's actual trained behavior* matches
what the task needs (RNNoise) — and neither was visible without pulling
real numbers and running a real, reproducible test.
