// The envelope report: candidate ADSR segment models, prototyped in
// envelope_candidates.hpp and measured to choose one (docs/dsp/envelopes.md).
// For each model:
//
// - the shape of each stage (levels at a quarter, half, and three quarters
//   of the stage's time; the release in dB, and how long it stays above
//   -60 dB);
// - timing accuracy: each stage's length against its time, at 44.1-192 kHz
//   and from 1 ms to 25 s (where float recursion runs out of precision), and
//   the worst deviation from the ideal curve;
// - stages entered part-way (a release from the sustain level, a retrigger
//   from the current level);
// - the discontinuity at each stage change and event, in level and slope,
//   and as a click: the envelope's energy above 1 kHz around the event,
//   relative to an instant step from 0 to 1;
// - attack, decay, and release of 0;
// - control changes mid-stage;
// - independence from the block size;
// - denormals in long tails.
//
// Hidden from the default run; run it in an optimised build (about 5 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[envelope-report]"
//
// CPU: the "[envelope]" benchmarks (tools/bench.sh "[envelope]").

#include "envelope_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>

#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::envelope;

namespace
{

constexpr std::array<float, 4> sampleRates{44100.0f, 48000.0f, 96000.0f, 192000.0f};

std::string fixed(double x, int digits)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::string decibels(double x, int digits = 1)
{
    if (x <= 0.0)
    {
        return "-inf";
    }
    return fixed(20.0 * std::log10(x), digits);
}

// Calls f.operator()<Model>(name, ratios) for every candidate, in the
// report's order.
template <typename F>
void forEachCandidate(const F& f)
{
    f.template operator()<LinearAccumulated>("Linear, accumulated", Ratios{});
    f.template operator()<LinearCounted>("Linear, counted", Ratios{});
    f.template operator()<ExponentialFloat>("Exponential r 0.3 / 0.3", Ratios{.attack = 0.3, .decayRelease = 0.3});
    f.template operator()<ExponentialFloat>("Exponential r 0.3 / 0.01", Ratios{.attack = 0.3, .decayRelease = 0.01});
    f.template operator()<ExponentialFloat>("Exponential r 0.3 / 0.001", Ratios{.attack = 0.3, .decayRelease = 0.001});
    f.template operator()<ExponentialDouble>("Exponential r 0.3 / 0.001, double",
                                             Ratios{.attack = 0.3, .decayRelease = 0.001});
    f.template operator()<ExponentialCounted>("Exponential r 0.3 / 0.001, counted",
                                              Ratios{.attack = 0.3, .decayRelease = 0.001});
    f.template operator()<AnalogRc>("Analog RC", Ratios{});
}

struct Adsr
{
    float attack = 0.01f;
    float decay = 0.1f;
    float sustain = 0.5f;
    float release = 0.1f;
};

template <typename Model>
Model make(Ratios ratios, float rate, const Adsr& adsr, bool fromCurrent = true)
{
    Model m{ratios};
    m.prepare(rate);
    m.setAttack(adsr.attack);
    m.setDecay(adsr.decay);
    m.setSustain(adsr.sustain);
    m.setRelease(adsr.release);
    m.setRetriggerFromCurrent(fromCurrent);
    return m;
}

enum class EventType : std::uint8_t
{
    gateOn,
    gateOff,
    attack,
    decay,
    sustain,
    release,
};

struct Event
{
    std::size_t at = 0;
    EventType type = EventType::gateOn;
    float value = 0.0f;
};

template <typename Model>
void apply(Model& m, const Event& e)
{
    switch (e.type)
    {
    case EventType::gateOn:
        m.gateOn();
        break;
    case EventType::gateOff:
        m.gateOff();
        break;
    case EventType::attack:
        m.setAttack(e.value);
        break;
    case EventType::decay:
        m.setDecay(e.value);
        break;
    case EventType::sustain:
        m.setSustain(e.value);
        break;
    case EventType::release:
        m.setRelease(e.value);
        break;
    }
}

// Renders numSamples in blocks of blockSize, applying each event before the
// sample it is at (events sorted by time). A block is split at events, as
// a voice would split it.
template <typename Model>
std::vector<float> render(Model& m, std::size_t numSamples, std::span<const Event> events, std::size_t blockSize)
{
    std::vector<float> out(numSamples);
    std::size_t next = 0;
    for (std::size_t start = 0; start < numSamples; start += blockSize)
    {
        const auto end = std::min(start + blockSize, numSamples);
        for (std::size_t n = start; n < end; ++n)
        {
            while (next < events.size() && events[next].at == n)
            {
                apply(m, events[next]);
                ++next;
            }
            out[n] = m.next();
        }
    }
    return out;
}

// The click of an event at `at`: the energy above 1 kHz of the envelope in
// a 4096-sample Hann window centred on it, relative to the same for an
// instant step from 0 to 1. 0 dB: as loud as a hard gate.
double clickDb(std::span<const float> y, std::size_t at, double rate)
{
    constexpr std::size_t size = 4096;
    const auto window = hannWindow(size);
    const auto above = [&](std::span<const float> x)
    {
        const auto spectrum = powerSpectrum(x, rate, window);
        double sum = 0.0;
        for (std::size_t k = spectrum.nearestBin(1000.0); k < spectrum.power.size(); ++k)
        {
            sum += spectrum.power[k];
        }
        return sum;
    };
    std::vector<float> step(size, 0.0f);
    std::fill(step.begin() + static_cast<std::ptrdiff_t>(size / 2), step.end(), 1.0f);
    const double reference = above(step);
    const double power = above(y.subspan(at - (size / 2), size));
    return power > 0.0 ? 10.0 * std::log10(power / reference) : -std::numeric_limits<double>::infinity();
}

std::string clickText(double db)
{
    return std::isfinite(db) ? fixed(db, 1) : std::string{"-inf"};
}

// The first index at or after `from` where pred(y) holds, or y.size().
template <typename Pred>
std::size_t firstWhere(std::span<const float> y, std::size_t from, Pred pred)
{
    for (std::size_t n = from; n < y.size(); ++n)
    {
        if (pred(y[n]))
        {
            return n;
        }
    }
    return y.size();
}

// --- Shape -----------------------------------------------------------------

void reportShape(std::ostringstream& out)
{
    out << "\n## Shape\n\n"
        << "48 kHz, every stage 1 s. Attack: the level at 25 / 50 / 75% of the attack time. Decay to sustain 0 "
           "and release from 1: the level in dB at 25 / 50 / 75% of the stage's time, and how long the release "
           "stays above -60 dB, as a fraction of its time.\n\n"
        << "| Model | Attack at 25 / 50 / 75% | Decay at 25 / 50 / 75% (dB) | Release at 25 / 50 / 75% (dB) | Release "
           "above -60 dB |\n"
        << "|---|---|---|---|---:|\n";
    constexpr float rate = 48000.0f;
    constexpr std::size_t length = 48000;
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            const std::array<std::size_t, 3> at{(length / 4) - 1, (length / 2) - 1, ((3 * length) / 4) - 1};

            auto a = make<Model>(ratios, rate, Adsr{.attack = 1.0f, .decay = 1.0f, .sustain = 1.0f, .release = 1.0f});
            const std::array<Event, 1> on{
                Event{.at = 0, .type = EventType::gateOn, .value = 0.0f},
            };
            const auto attack = render(a, length, on, 512);

            auto d = make<Model>(ratios, rate, Adsr{.attack = 0.0f, .decay = 1.0f, .sustain = 0.0f, .release = 1.0f});
            const auto decay = render(d, length, on, 512);

            auto r = make<Model>(ratios, rate, Adsr{.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 1.0f});
            const std::array<Event, 2> onOff{
                Event{.at = 0, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = 1, .type = EventType::gateOff, .value = 0.0f},
            };
            const auto release = render(r, length + 1, onOff, 512);
            const auto above = firstWhere(std::span{release}.subspan(1), 0, [](float y) { return y < 1e-3f; });

            out << "| " << name << " | " << fixed(attack[at[0]], 3) << " / " << fixed(attack[at[1]], 3) << " / "
                << fixed(attack[at[2]], 3) << " | " << decibels(decay[at[0]]) << " / " << decibels(decay[at[1]])
                << " / " << decibels(decay[at[2]]) << " | " << decibels(release[at[0] + 1]) << " / "
                << decibels(release[at[1] + 1]) << " / " << decibels(release[at[2] + 1]) << " | "
                << fixed(static_cast<double>(above) / static_cast<double>(length), 3) << " |\n";
        });
}

// --- Timing ----------------------------------------------------------------

// The ideal attack level after k samples of an N-sample attack.
double idealAttack(const char* name, Ratios ratios, double k, double n)
{
    const std::string s{name};
    if (s.starts_with("Linear"))
    {
        return k / n;
    }
    const double r = s.starts_with("Analog") ? rcAttackRatio : ratios.attack;
    const double c = std::pow(r / (1.0 + r), 1.0 / n);
    return (1.0 + r) * (1.0 - std::pow(c, k));
}

void reportTiming(std::ostringstream& out)
{
    out << "\n## Timing\n\n"
        << "Each stage's length in samples against round(time x rate), at 44.1, 48, 96, and 192 kHz: the worst "
           "error over the four rates, attack / release (release from 1; the analog RC's to its -80 dB end). "
           "Then the worst deviation of the attack from its ideal curve (computed in double), over the four "
           "rates.\n\n"
        << "| Model | 1 ms | 100 ms | 1 s | 25 s | Attack curve error at 1 s / 25 s |\n"
        << "|---|---|---|---|---|---|\n";
    constexpr std::array<float, 4> times{0.001f, 0.1f, 1.0f, 25.0f};
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            out << "| " << name << " |";
            std::array<double, 2> curveError{};
            for (std::size_t t = 0; t < times.size(); ++t)
            {
                std::int64_t worstAttack = 0;
                std::int64_t worstRelease = 0;
                for (const float rate : sampleRates)
                {
                    const auto expected = static_cast<std::int64_t>(
                        std::round(static_cast<double>(times[t]) * static_cast<double>(rate)));
                    const auto length = (static_cast<std::size_t>(expected) * 2) + 16;

                    auto a = make<Model>(ratios, rate,
                                         Adsr{.attack = times[t], .decay = 1.0f, .sustain = 1.0f, .release = 1.0f});
                    a.gateOn();
                    std::int64_t attackLength = -1;
                    double worstCurve = 0.0;
                    for (std::size_t n = 0; n < length; ++n)
                    {
                        const float y = a.next();
                        if (n + 1 < static_cast<std::size_t>(expected))
                        {
                            const double ideal =
                                idealAttack(name, ratios, static_cast<double>(n + 1), static_cast<double>(expected));
                            worstCurve = std::max(worstCurve, std::abs(static_cast<double>(y) - ideal));
                        }
                        if (y >= 1.0f)
                        {
                            attackLength = static_cast<std::int64_t>(n) + 1;
                            break;
                        }
                    }
                    if (t >= 2)
                    {
                        curveError[t - 2] = std::max(curveError[t - 2], worstCurve);
                    }

                    auto r = make<Model>(ratios, rate,
                                         Adsr{.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = times[t]});
                    r.gateOn();
                    static_cast<void>(r.next());
                    r.gateOff();
                    std::int64_t releaseLength = -1;
                    for (std::size_t n = 0; n < length; ++n)
                    {
                        static_cast<void>(r.next());
                        if (!r.isActive())
                        {
                            releaseLength = static_cast<std::int64_t>(n) + 1;
                            break;
                        }
                    }
                    const auto worse = [](std::int64_t worst, std::int64_t error)
                    {
                        return std::abs(error) > std::abs(worst) ? error : worst;
                    };
                    worstAttack = worse(worstAttack, attackLength < 0 ? -expected : attackLength - expected);
                    worstRelease = worse(worstRelease, releaseLength < 0 ? -expected : releaseLength - expected);
                }
                out << " " << worstAttack << " / " << worstRelease << " |";
            }
            out << " " << std::scientific << std::setprecision(1) << curveError[0] << " / " << curveError[1]
                << std::defaultfloat << " |\n";
        });
    out << "\nAn error is in samples; +k ends k samples late. -N would mean the stage never ended.\n";
}

// --- Stages entered part-way ---------------------------------------------------

void reportPartial(std::ostringstream& out)
{
    out << "\n## Stages entered part-way\n\n"
        << "48 kHz, every time 1 s. The release from a sustain level of 0.5 and 0.1, and the attack retriggered "
           "from 0.5 (from the current level), each as a fraction of the stage's time.\n\n"
        << "| Model | Release from 0.5 | Release from 0.1 | Attack from 0.5 |\n"
        << "|---|---:|---:|---:|\n";
    constexpr float rate = 48000.0f;
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            const auto releaseFrom = [&](float sustain)
            {
                auto m =
                    make<Model>(ratios, rate, Adsr{.attack = 0.0f, .decay = 0.0f, .sustain = sustain, .release = 1.0f});
                m.gateOn();
                static_cast<void>(m.next());
                m.gateOff();
                std::size_t n = 0;
                while (m.isActive() && n < 200000)
                {
                    static_cast<void>(m.next());
                    ++n;
                }
                return static_cast<double>(n) / static_cast<double>(rate);
            };
            // Attack from 0.5: release from 1 to 0.5, then retrigger.
            auto m = make<Model>(ratios, rate, Adsr{.attack = 1.0f, .decay = 0.0f, .sustain = 1.0f, .release = 0.001f});
            m.setAttack(0.0f);
            m.gateOn();
            static_cast<void>(m.next());
            m.setRelease(1.0f);
            m.gateOff();
            while (m.next() > 0.5f)
            {
            }
            m.setAttack(1.0f);
            m.gateOn();
            std::size_t n = 0;
            while (m.next() < 1.0f && n < 200000)
            {
                ++n;
            }
            out << "| " << name << " | " << fixed(releaseFrom(0.5f), 3) << " | " << fixed(releaseFrom(0.1f), 3) << " | "
                << fixed(static_cast<double>(n + 1) / static_cast<double>(rate), 3) << " |\n";
        });
}

// --- Discontinuities -------------------------------------------------------------

struct Discontinuity
{
    double levelStep = 0.0;   // y[at] - y[at - 1], minus the slope before
    double slopeBefore = 0.0; // per ms
    double slopeAfter = 0.0;  // per ms
    double click = 0.0;       // dB re a hard 0 -> 1 step
};

Discontinuity measureAt(std::span<const float> y, std::size_t at, double rate)
{
    const double perMs = rate / 1000.0;
    const double before = static_cast<double>(y[at - 1]) - static_cast<double>(y[at - 2]);
    const double after = static_cast<double>(y[at + 1]) - static_cast<double>(y[at]);
    const double step = static_cast<double>(y[at]) - static_cast<double>(y[at - 1]);
    return {
        .levelStep = step - before,
        .slopeBefore = before * perMs,
        .slopeAfter = after * perMs,
        .click = clickDb(y, at, rate),
    };
}

std::string describe(const Discontinuity& d)
{
    return fixed(d.levelStep, 4) + " \\| " + fixed(d.slopeBefore, 3) + " -> " + fixed(d.slopeAfter, 3) + " \\| " +
           clickText(d.click);
}

void reportDiscontinuities(std::ostringstream& out)
{
    constexpr float rate = 48000.0f;
    constexpr double rateD = 48000.0;
    const Adsr adsr{.attack = 0.01f, .decay = 0.1f, .sustain = 0.5f, .release = 0.1f};
    constexpr std::size_t gate = 4096;
    const std::size_t peakAt = gate + 480; // the attack's 480th sample is its peak
    constexpr std::size_t length = 48000;

    out << "\n## Discontinuities at stage changes and events\n\n"
        << "48 kHz, A 10 ms, D 100 ms, S 0.5, R 100 ms. For each event: the level step beyond the slope before "
           "it, the slope before -> after (full scale per ms), and the click (energy above 1 kHz in a 4096-sample "
           "Hann window centred on the event, in dB relative to an instant 0 -> 1 step; a hard gate is 0 dB).\n\n";

    out << "Reference: an instant gate (attack 0) measures 0.0 dB by definition.\n\n";

    out << "### Attack peak and gate-off\n\n"
        << "| Model | Attack -> decay: step \\| slope \\| click | Gate-off mid-attack (5 ms): step \\| slope \\| "
           "click | Gate-off mid-decay (50 ms): step \\| slope \\| click |\n"
        << "|---|---|---|---|\n";
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            auto a = make<Model>(ratios, rate, adsr);
            const std::array<Event, 1> on{
                Event{.at = gate, .type = EventType::gateOn, .value = 0.0f},
            };
            const auto y = render(a, length, on, 512);

            auto b = make<Model>(ratios, rate, adsr);
            const std::array<Event, 2> offAttack{
                Event{.at = gate, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = gate + 240, .type = EventType::gateOff, .value = 0.0f},
            };
            const auto z = render(b, length, offAttack, 512);

            auto c = make<Model>(ratios, rate, adsr);
            const std::array<Event, 2> offDecay{
                Event{.at = gate, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = peakAt + 2400, .type = EventType::gateOff, .value = 0.0f},
            };
            const auto w = render(c, length, offDecay, 512);

            out << "| " << name << " | " << describe(measureAt(y, peakAt, rateD)) << " | "
                << describe(measureAt(z, gate + 240, rateD)) << " | " << describe(measureAt(w, peakAt + 2400, rateD))
                << " |\n";
        });

    out << "\n### Retrigger mid-release, and a sustain change\n\n"
        << "The retrigger comes 25 ms into the release from 0.5 (at a level between 0.03 and 0.25, by model). The "
           "sustain change is 0.5 -> 0.25 in the sustain stage: the prototypes jump, except the analog RC, whose "
           "decay runs on to the new level. For comparison, the same jump ramped linearly over 10 ms "
           "(`LinearSmoother`) measures "
        <<
        [&]
    {
        anoesis::dsp::LinearSmoother s;
        s.prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 1});
        s.setRampTime(0.01f);
        s.setCurrentAndTarget(0.5f);
        std::vector<float> y(8192);
        for (std::size_t n = 0; n < y.size(); ++n)
        {
            if (n == 4096)
            {
                s.setTarget(0.25f);
            }
            y[n] = s.next();
        }
        return clickText(clickDb(y, 4096, rateD));
    }() << " dB, and an instant jump of 0.25 measures "
        << clickText(10.0 * std::log10(0.25 * 0.25)) << " dB.\n\n"
        << "| Model | Level at the retrigger | Retrigger from 0: step \\| click | Retrigger from the current level: "
           "step \\| click | Sustain 0.5 -> 0.25: step \\| click |\n"
        << "|---|---:|---|---|---|\n";
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            const std::size_t off = peakAt + 9600; // well into the sustain stage
            const std::size_t again = off + 1200;
            const std::array<Event, 3> events{
                Event{.at = gate, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = off, .type = EventType::gateOff, .value = 0.0f},
                Event{.at = again, .type = EventType::gateOn, .value = 0.0f},
            };
            auto zero = make<Model>(ratios, rate, adsr, false);
            const auto y0 = render(zero, length, events, 512);
            auto current = make<Model>(ratios, rate, adsr, true);
            const auto y1 = render(current, length, events, 512);

            const std::array<Event, 2> change{
                Event{.at = gate, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = off, .type = EventType::sustain, .value = 0.25f},
            };
            auto s = make<Model>(ratios, rate, adsr);
            const auto y2 = render(s, length, change, 512);

            const auto d0 = measureAt(y0, again, rateD);
            const auto d1 = measureAt(y1, again, rateD);
            const auto d2 = measureAt(y2, off, rateD);
            out << "| " << name << " | " << fixed(y1[again - 1], 4) << " | " << fixed(d0.levelStep, 4) << " \\| "
                << clickText(d0.click) << " | " << fixed(d1.levelStep, 4) << " \\| " << clickText(d1.click) << " | "
                << fixed(d2.levelStep, 4) << " \\| " << clickText(d2.click) << " |\n";
        });
}

// --- Zero-length stages ----------------------------------------------------------

void reportZeroLength(std::ostringstream& out)
{
    out << "\n## Attack, decay, and release of 0\n\n"
        << "48 kHz, otherwise A 10 ms, D 100 ms, S 0.5, R 100 ms. A 0: the output on the gate's sample. D 0: the "
           "sample after the attack's peak. R 0: the output on the gate-off sample, and whether the envelope is "
           "then idle.\n\n"
        << "| Model | A 0: gate sample | D 0: after the peak | R 0: gate-off sample, idle |\n"
        << "|---|---:|---:|---|\n";
    constexpr float rate = 48000.0f;
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            auto a = make<Model>(ratios, rate, Adsr{.attack = 0.0f, .decay = 0.1f, .sustain = 0.5f, .release = 0.1f});
            a.gateOn();
            const float first = a.next();

            auto d = make<Model>(ratios, rate, Adsr{.attack = 0.01f, .decay = 0.0f, .sustain = 0.5f, .release = 0.1f});
            d.gateOn();
            for (int n = 0; n < 480; ++n)
            {
                static_cast<void>(d.next());
            }
            const float afterPeak = d.next();

            auto r = make<Model>(ratios, rate, Adsr{.attack = 0.01f, .decay = 0.1f, .sustain = 0.5f, .release = 0.0f});
            r.gateOn();
            for (int n = 0; n < 9600; ++n)
            {
                static_cast<void>(r.next());
            }
            r.gateOff();
            const float off = r.next();

            out << "| " << name << " | " << fixed(first, 6) << " | " << fixed(afterPeak, 6) << " | " << fixed(off, 6)
                << ", " << (r.isActive() ? "active" : "idle") << " |\n";
        });
}

// --- Control changes mid-stage ---------------------------------------------------

void reportControlChanges(std::ostringstream& out)
{
    out << "\n## Control changes mid-stage\n\n"
        << "48 kHz. The attack time changed from 100 ms to 10 ms at 50 ms into the attack; the release time from "
           "1 s to 100 ms at 100 ms into a release from 1. The level step beyond the slope before the change, and "
           "the time the stage then takes to finish (ms).\n\n"
        << "| Model | Attack: step \\| remaining | Release: step \\| remaining |\n"
        << "|---|---|---|\n";
    constexpr float rate = 48000.0f;
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            const auto remaining = [&](Model& m, std::span<const Event> events, std::size_t at, auto done)
            {
                const auto y = render(m, at + 2, events, 512);
                const double step = (static_cast<double>(y[at]) - static_cast<double>(y[at - 1])) -
                                    (static_cast<double>(y[at - 1]) - static_cast<double>(y[at - 2]));
                std::size_t n = 2;
                while (!done(m) && n < 200000)
                {
                    static_cast<void>(m.next());
                    ++n;
                }
                return fixed(step, 5) + " \\| " + fixed(static_cast<double>(n) * 1000.0 / 48000.0, 2);
            };
            auto a = make<Model>(ratios, rate, Adsr{.attack = 0.1f, .decay = 1.0f, .sustain = 0.5f, .release = 1.0f});
            const std::array<Event, 2> attack{
                Event{.at = 2, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = 2402, .type = EventType::attack, .value = 0.01f},
            };
            const auto attackText =
                remaining(a, attack, 2402, [](const Model& m) { return m.stage() != Stage::attack; });

            auto r = make<Model>(ratios, rate, Adsr{.attack = 0.0f, .decay = 0.0f, .sustain = 1.0f, .release = 1.0f});
            const std::array<Event, 3> release{
                Event{.at = 2, .type = EventType::gateOn, .value = 0.0f},
                Event{.at = 3, .type = EventType::gateOff, .value = 0.0f},
                Event{.at = 4803, .type = EventType::release, .value = 0.1f},
            };
            const auto releaseText = remaining(r, release, 4803, [](const Model& m) { return !m.isActive(); });
            out << "| " << name << " | " << attackText << " | " << releaseText << " |\n";
        });
}

// --- Block sizes ----------------------------------------------------------------

void reportBlockSizes(std::ostringstream& out)
{
    out << "\n## Block sizes\n\n"
        << "One script of gates and control changes at fixed samples (a note, a retrigger mid-decay, a sustain "
           "change, time changes mid-stage, a release, a retrigger mid-release), rendered at 48 kHz in blocks of "
           "1, 37, and 4096 samples, events applied at their sample. Bit-identical?\n\n"
        << "| Model | 1 = 37 = 4096 |\n"
        << "|---|---|\n";
    constexpr float rate = 48000.0f;
    const std::array<Event, 9> script{
        Event{.at = 100, .type = EventType::gateOn, .value = 0.0f},
        Event{.at = 2000, .type = EventType::decay, .value = 0.05f},
        Event{.at = 5000, .type = EventType::gateOn, .value = 0.0f},
        Event{.at = 9000, .type = EventType::sustain, .value = 0.3f},
        Event{.at = 12000, .type = EventType::attack, .value = 0.02f},
        Event{.at = 20000, .type = EventType::gateOff, .value = 0.0f},
        Event{.at = 21000, .type = EventType::release, .value = 0.3f},
        Event{.at = 24000, .type = EventType::gateOn, .value = 0.0f},
        Event{.at = 40000, .type = EventType::gateOff, .value = 0.0f},
    };
    forEachCandidate(
        [&]<typename Model>(const char* name, Ratios ratios)
        {
            bool same = true;
            auto m1 = make<Model>(ratios, rate, {});
            const auto reference = render(m1, 60000, script, 1);
            for (const std::size_t block : {std::size_t{37}, std::size_t{4096}})
            {
                auto m = make<Model>(ratios, rate, {});
                const auto y = render(m, 60000, script, block);
                for (std::size_t n = 0; n < y.size(); ++n)
                {
                    same = same && std::bit_cast<std::uint32_t>(y[n]) == std::bit_cast<std::uint32_t>(reference[n]);
                }
            }
            out << "| " << name << " | " << (same ? "yes" : "**no**") << " |\n";
        });
}

// --- Denormals ------------------------------------------------------------------

bool isSubnormal(double x)
{
    const double a = std::abs(x);
    return a > 0.0 && a < static_cast<double>(std::numeric_limits<float>::min());
}

template <typename Model>
void denormalRow(std::ostringstream& out, const char* name, Ratios ratios)
{
    constexpr float rate = 48000.0f;
    constexpr std::size_t minute = std::size_t{48000} * 60;
    const auto count = [&](float sustain, bool release)
    {
        auto m = make<Model>(ratios, rate, Adsr{.attack = 0.0f, .decay = 0.01f, .sustain = sustain, .release = 1.0f});
        m.gateOn();
        std::size_t outputs = 0;
        std::size_t states = 0;
        double first = -1.0;
        for (std::size_t n = 0; n < minute; ++n)
        {
            if (release && n == 48)
            {
                m.gateOff();
            }
            const float y = m.next();
            const bool subOut = isSubnormal(static_cast<double>(y));
            const bool subState = isSubnormal(m.state());
            outputs += subOut ? 1 : 0;
            states += subState ? 1 : 0;
            if ((subOut || subState) && first < 0.0)
            {
                first = static_cast<double>(n) / static_cast<double>(rate);
            }
        }
        return std::to_string(outputs) + " / " + std::to_string(states) +
               (first >= 0.0 ? " (from " + fixed(first * 1000.0, 0) + " ms)" : std::string{});
    };
    out << "| " << name << " | " << count(0.0f, false) << " | " << count(0.5f, false) << " | " << count(0.5f, true)
        << " |\n";
}

void reportDenormals(std::ostringstream& out)
{
    out << "\n## Denormals in long tails\n\n"
        << "48 kHz, one minute, with no flush-to-zero mode set. Subnormal outputs / subnormal internal states "
           "(the level, or the distance to the target), and when the first appears. A 10 ms decay held at "
           "sustain 0 and at sustain 0.5; a 1 s release from 0.5.\n\n"
        << "| Model | Decay to 0, held | Decay to 0.5, held | Release |\n"
        << "|---|---|---|---|\n";
    forEachCandidate([&]<typename Model>(const char* name, Ratios ratios) { denormalRow<Model>(out, name, ratios); });
    denormalRow<AnalogRcFlushed>(out, "Analog RC, state flushed", Ratios{});
}

} // namespace

TEST_CASE("Envelope report: candidate segment models", "[.][envelope-report]")
{
    std::ostringstream out;
    out << "# Envelope report\n";
    reportShape(out);
    reportTiming(out);
    reportPartial(out);
    reportDiscontinuities(out);
    reportZeroLength(out);
    reportControlChanges(out);
    reportBlockSizes(out);
    reportDenormals(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}
