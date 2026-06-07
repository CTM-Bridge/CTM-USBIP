struct CtmPcmReservoir {
    mutable std::mutex mu;
    std::condition_variable dataCv;
    std::deque<int16_t> samples;
    uint32_t sampleRate = 48000;
    uint8_t channels = 4;
    size_t maxSamples = 0;
    size_t warmupSamples = 0;
    bool warmed = false;
    std::atomic<bool> stopRequested{false};
    std::atomic<uint64_t> dropOldestEvents{0};
    std::atomic<uint64_t> consumerWaits{0};

    void configure(uint32_t sampleRateHz, uint8_t channelCount, uint32_t maxMs, uint32_t warmupMs)
    {
        sampleRate = sampleRateHz == 0 ? 48000 : sampleRateHz;
        channels = channelCount == 0 ? 1 : channelCount;
        const uint64_t maxFrames = static_cast<uint64_t>(maxMs) * sampleRate / 1000ULL;
        const uint64_t warmupFrames = static_cast<uint64_t>(warmupMs) * sampleRate / 1000ULL;
        maxSamples = static_cast<size_t>(maxFrames * channels);
        warmupSamples = static_cast<size_t>(warmupFrames * channels);
        warmed = warmupSamples == 0;
        stopRequested.store(false, std::memory_order_relaxed);
    }

    void push(const int16_t *data, size_t count)
    {
        if (data == nullptr || count == 0) {
            return;
        }
        std::unique_lock<std::mutex> lock(mu);
        if (samples.size() + count > maxSamples && maxSamples > 0) {
            const size_t overflow = (samples.size() + count) - maxSamples;
            const size_t toDrop = (std::min)(overflow, samples.size());
            for (size_t i = 0; i < toDrop; ++i) {
                samples.pop_front();
            }
            dropOldestEvents.fetch_add(1, std::memory_order_relaxed);
        }
        samples.insert(samples.end(), data, data + count);
        if (!warmed && samples.size() >= warmupSamples) {
            warmed = true;
        }
        dataCv.notify_all();
    }

    bool pull(size_t frameCount, int16_t *out)
    {
        if (out == nullptr || frameCount == 0) {
            return false;
        }
        const size_t need = frameCount * channels;
        std::unique_lock<std::mutex> lock(mu);
        if (!(warmed && samples.size() >= need) && !stopRequested.load(std::memory_order_relaxed)) {
            consumerWaits.fetch_add(1, std::memory_order_relaxed);
        }
        dataCv.wait(lock, [&]() {
            return stopRequested.load(std::memory_order_relaxed) || (warmed && samples.size() >= need);
        });
        if (stopRequested.load(std::memory_order_relaxed) && samples.size() < need) {
            return false;
        }
        for (size_t i = 0; i < need; ++i) {
            out[i] = samples.front();
            samples.pop_front();
        }
        return true;
    }

    uint32_t fill_ms() const
    {
        std::lock_guard<std::mutex> lock(mu);
        const size_t frames = channels == 0 ? 0 : samples.size() / channels;
        return sampleRate == 0
            ? 0
            : static_cast<uint32_t>(static_cast<uint64_t>(frames) * 1000ULL / sampleRate);
    }

    void request_stop()
    {
        stopRequested.store(true, std::memory_order_relaxed);
        dataCv.notify_all();
    }
};

static size_t ctm_resample_interleaved_linear(
    const int16_t *in,
    size_t inFrames,
    uint32_t srcRate,
    uint32_t dstRate,
    uint8_t channels,
    int16_t *out)
{
    if (in == nullptr || out == nullptr || inFrames == 0 || srcRate == 0 || dstRate == 0 || channels == 0) {
        return 0;
    }
    if (srcRate == dstRate) {
        const size_t samples = inFrames * channels;
        memcpy(out, in, samples * sizeof(int16_t));
        return inFrames;
    }
    if (inFrames == 1) {
        for (uint8_t c = 0; c < channels; ++c) {
            out[c] = in[c];
        }
        return 1;
    }
    const uint64_t dstFrames64 =
        static_cast<uint64_t>(inFrames) * static_cast<uint64_t>(dstRate) /
        static_cast<uint64_t>(srcRate);
    const size_t dstFrames = static_cast<size_t>((std::max<uint64_t>)(1, dstFrames64));
    for (size_t i = 0; i < dstFrames; ++i) {
        const uint64_t scaled = static_cast<uint64_t>(i) * static_cast<uint64_t>(srcRate);
        const size_t idx = static_cast<size_t>(scaled / static_cast<uint64_t>(dstRate));
        const uint64_t rem = scaled - static_cast<uint64_t>(idx) * static_cast<uint64_t>(dstRate);
        const double frac = static_cast<double>(rem) / static_cast<double>(dstRate);
        const size_t i0 = idx;
        const size_t i1 = (i0 + 1 < inFrames) ? (i0 + 1) : i0;
        for (uint8_t c = 0; c < channels; ++c) {
            const int32_t a = in[i0 * channels + c];
            const int32_t b = in[i1 * channels + c];
            const double v = static_cast<double>(a) + frac * static_cast<double>(b - a);
            int32_t vi = static_cast<int32_t>(v >= 0 ? v + 0.5 : v - 0.5);
            if (vi > 32767) vi = 32767;
            if (vi < -32768) vi = -32768;
            out[i * channels + c] = static_cast<int16_t>(vi);
        }
    }
    return dstFrames;
}
