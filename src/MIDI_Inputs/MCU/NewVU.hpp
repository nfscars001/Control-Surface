#pragma once

#include <AH/Timing/MillisMicrosTimer.hpp>
#include <MIDI_Inputs/MIDIInputElementMatchers.hpp>

BEGIN_CS_NAMESPACE

namespace MCU {

/// Struct that keeps track of the value and overload indicator of a Mackie
/// Control Universal VU meter.
struct VUState {
    /**
     * @brief   Constructor.
     * 
     * @param   value 
     *          The value of the VU meter [0, 12].
     * @param   overload 
     *          The state of the overload indicator.
     */
    VUState(uint8_t value = 0, bool overload = false)
        : value(value), overload(overload) {}

    uint8_t value : 4; ///< The value of the VU meter [0, 12].
    bool overload : 1; ///< The state of the overload indicator.

    enum Changed {
        NothingChanged = 0,
        ValueChanged = 1,
        OverloadChanged = 2,
    };

    /// Update the value or overload status with a new raw MIDI value.
    ///
    /// @param  data
    ///         The raw 4-bit MIDI data (with track number masked out).
    ///
    /// @retval ValueChanged
    ///         The VU meter value has changed.
    /// @retval OverloadChanged
    ///         The overload status has changed.
    /// @retval NothingChanged
    ///         Neither the value nor overload status has changed.
    Changed update(uint8_t data) {
        switch (data) {
            case 0xF: { // clear overload
                Changed changed = overload ? OverloadChanged : NothingChanged;
                overload = false;
                return changed;
            }
            case 0xE: { // set overload
                Changed changed = !overload ? OverloadChanged : NothingChanged;
                overload = true;
                return changed;
            }
            case 0xD: { // no meaning
                return NothingChanged;
            }
            default: { // set value
                Changed changed = value != data ? ValueChanged : NothingChanged;
                value = data;
                return changed;
            }
        }
    }

    /// Decay the VU value: subtract one from the position if it is not zero.
    /// @return     Returns true if the value changed.
    bool decay() {
        if (value == 0)
            return false;
        value--;
        return true;
    }
};

// -------------------------------------------------------------------------- //

/**
 * @brief   MIDI Input matcher for Mackie Control Universal VU meters.
 *
 * In the Mackie Control Universal protocol, VU meters are updated using Channel
 * Pressure events.  
 * Each device (cable number) has eight VU meters for the eight tracks. Only
 * MIDI channel 1 is used in the original protocol.
 * 
 * The format of the MIDI message is as follows:
 * | Status      | Data 1      |
 * |:-----------:|:-----------:|
 * | `1101 cccc` | `0hhh llll` |
 * 
 * - `1101` (or `0xD`) is the status for Channel Pressure events
 * - `cccc` is the MIDI channel [0-15]
 * - `hhh` is the track index [0-7]
 * - `llll` is the level of the VU meter
 * 
 * If the level is `0x0`, the meter is at 0%, if it's `0xC`, the meter is at 
 * 100%.  
 * `0xD` is an invalid value.  
 * `0xE` sets the overload indicator, and `0xF` clears the overload indicator.
 */
struct VUMatcher {
    /// Constructor.
    VUMatcher(MIDIAddress address) : address(address) {}

    /// Output data of the matcher/parser.
    struct Result {
        bool match; ///< Whether the address of the message matched our address.
        uint8_t data; ///< The data to update the VU meter with [0x0, 0xF].
    };

    /// Parse and try to match the incoming MIDI message.
    Result operator()(ChannelMessageMatcher m) {
        uint8_t track = m.data1 >> 4;
        if (!MIDIAddress::matchSingle({track, m.getChannelCN()}, address))
            return {false, 0};
        uint8_t data = m.data1 & 0x0F;
        return {true, data};
    }

    MIDIAddress address; ///< MIDI address to compare incoming messages with.
};

namespace Bankable {

/// MIDI Input matcher for Mackie Control Universal VU meters with bank support.
/// @see    MCU::VUMatcher
template <uint8_t BankSize>
struct VUMatcher {
    /// Constructor.
    VUMatcher(BankConfig<BankSize> config, MIDIAddress address)
        : config(config), address(address) {}

    /// Output data of the matcher/parser.
    struct Result {
        bool match; ///< Whether the address of the message matched our address.
        uint8_t data;      ///< The data to update the VU meter with [0x0, 0xF].
        uint8_t bankIndex; ///< Bank index of the message [0, BankSize-1].
    };

    /// Parse and try to match the incoming MIDI message.
    Result operator()(ChannelMessageMatcher m) {
        using BankableMIDIMatcherHelpers::getBankIndex;
        using BankableMIDIMatcherHelpers::matchBankable;
        uint8_t track = m.data1 >> 4;
        MIDIAddress midiaddr = {track, m.getChannelCN()};
        if (!matchBankable(midiaddr, address, config))
            return {false, 0, 0};
        uint8_t data = m.data1 & 0x0F;
        uint8_t bankIndex = getBankIndex(midiaddr, address, config);
        return {true, data, bankIndex};
    }

    Bank<BankSize> &getBank() { return config.bank; }
    const Bank<BankSize> &getBank() const { return config.bank; }
    BankType getBankType() const { return config.type; }
    static constexpr setting_t getBankSize() { return BankSize; }

    /// Get the current bank setting.
    /// @see    @ref Bank<N>::getSelection()
    setting_t getSelection() const { return getBank().getSelection(); }

    BaseBankConfig<BankSize> config; ///< Bank configuration.
    MIDIAddress address; ///< MIDI address to compare incoming messages with.
};

} // namespace Bankable

// -------------------------------------------------------------------------- //

/// VU Decay time constants.
namespace VUDecay {
/// Don't decay automatically, hold the latest value until a new one is received.
constexpr unsigned int Hold = 0;
/// Decay one segment/block every 150 ms if no new values are received.
constexpr unsigned int Default = 150;
} // namespace VUDecay

/** 
 * @brief   A MIDI input element that represents a Mackie Control Universal VU
 *          meter.  
 */
class NewVU : public MatchingMIDIInputElement<MIDIMessageType::CHANNEL_PRESSURE,
                                              VUMatcher> {
  public:
    /**
     * @brief   Constructor.
     * 
     * @param   track
     *          The track of the VU meter. [1, 8]
     * @param   channelCN
     *          The MIDI channel [CHANNEL_1, CHANNEL_16] and optional Cable
     *          Number [CABLE_1, CABLE_16].
     * @param   decayTime
     *          The time in milliseconds it takes for the value to decay one
     *          step.  
     *          The MCU protocol uses 300 ms per division, and two steps
     *          per division, so the default is 150 ms per step.  
     *          Some software doesn't work if the VU meter decays automatically, 
     *          in that case, you can set the decay time to zero to disable 
     *          the decay.
     *          @see    @ref MCU::VUDecay
     */
    NewVU(uint8_t track, MIDIChannelCN channel,
          unsigned int decayTime = VUDecay::Default)
        : MatchingMIDIInputElement<MIDIMessageType::CHANNEL_PRESSURE,
                                   VUMatcher>({{track - 1, channel}}),
          decayTimer(decayTime) {}
    /**
     * @brief   Constructor.
     * 
     * @param   track
     *          The track of the VU meter. [1, 8]
     * @param   decayTime
     *          The time in milliseconds it takes for the value to decay one
     *          step.  
     *          The MCU protocol uses 300 ms per division, and two steps
     *          per division, so the default is 150 ms per step.  
     *          Some software doesn't work if the VU meter decays automatically, 
     *          in that case, you can set the decay time to zero to disable 
     *          the decay.
     *          @see    @ref MCU::VUDecay
     */
    NewVU(uint8_t track, unsigned int decayTime = VUDecay::Default)
        : NewVU(track, CHANNEL_1, decayTime) {}

  protected:
    void handleUpdate(VUMatcher::Result match) override {
        auto changed = state.update(match.data);
        if (changed) {
            if (changed == VUState::ValueChanged)
                // reset the timer and fire after one interval
                decayTimer.beginNextPeriod();
            dirty = true;
        }
    }

  public:
    /// Get the most recent VU position that was received.
    uint8_t getPosition() const { return state.value; }
    /// Get the status of the overload indicator.
    bool getOverload() const { return state.overload; }

    /// @copydoc    getPosition
    uint8_t getValue() const { return getPosition(); }
    /// Get the most recent VU position as a value between 0 and 1.
    float getFloatValue() const { return getPosition() / 12.f; }

    /// Reset all values to zero.
    void reset() override { state = {}; }

    /// Decay the VU meter.
    void update() override {
        if (decayTimer.getInterval() != VUDecay::Hold && decayTimer)
            dirty |= state.decay();
    }

    /// Check if the value was updated since the last time the dirty flag was
    /// cleared.
    bool getDirty() const { return dirty; }
    /// Clear the dirty flag.
    void clearDirty() { dirty = false; }

  private:
    VUState state = {};
    bool dirty = true;
    AH::Timer<millis> decayTimer;
};

// -------------------------------------------------------------------------- //

namespace Bankable {

/// @tparam  BankSize
///          The number of banks.
template <uint8_t BankSize>
class NewVU
    : public BankableMatchingMIDIInputElement<MIDIMessageType::CHANNEL_PRESSURE,
                                              VUMatcher<BankSize>> {
  public:
    /// Constructor.
    ///
    /// @param  config
    ///         The bank configuration to use.
    /// @param  track
    ///         The track to listen for [1, 8].
    /// @param  channel
    ///         The MIDI channel and cable.
    NewVU(BankConfig<BankSize> config, uint8_t track, MIDIChannelCN channel,
          unsigned int decayTime = VUDecay::Default)
        : BankableMatchingMIDIInputElement<MIDIMessageType::CHANNEL_PRESSURE,
                                           VUMatcher<BankSize>>(
              {config, {track - 1, channel}}),
          decayTimer(decayTime) {}

  protected:
    void handleUpdate(typename VUMatcher<BankSize>::Result match) override {
        auto changed = states[match.bankIndex].update(match.data);
        if (changed) {
            if (changed == VUState::ValueChanged &&
                match.bankIndex == getActiveBank())
                // Only care about active bank's decay.
                // Other banks will decay as well, but not as precisely.
                // They aren't visible anyway, so it's a good compromise.
                decayTimer.beginNextPeriod();
            dirty = true;
        }
    }

    uint8_t getActiveBank() const { return this->matcher.getSelection(); }

  public:
    /// Get the most recent VU position that was received for the active bank.
    uint8_t getPosition() const { return states[getActiveBank()].value; }
    /// Get the status of the overload indicator for the active bank.
    bool getOverload() const { return states[getActiveBank()].overload; }

    /// Get the most recent VU position that was received for the given bank.
    uint8_t getPosition(uint8_t bank) const { return states[bank].value; }
    /// Get the status of the overload indicator for the given bank.
    bool getOverload(uint8_t bank) const { return states[bank].overload; }

    /// @copydoc    getPosition
    uint8_t getValue() const { return getPosition(); }
    /// Get the most recent VU position as a value between 0 and 1.
    float getFloatValue() const { return getPosition() / 12.f; }

    /// Reset all values to zero.
    void reset() override {
        states = {{}};
        dirty = true;
    }

    /// Decay the VU meter.
    void update() override {
        if (decayTimer.getInterval() != VUDecay::Hold && decayTimer)
            for (uint8_t i = 0; i < BankSize; ++i)
                dirty |= states[i].decay() && i == getActiveBank();
        // Only mark dirty if the value of the active bank decayed
    }

    /// Check if the value was updated since the last time the dirty flag was
    /// cleared.
    bool getDirty() const { return dirty; }
    /// Clear the dirty flag.
    void clearDirty() { dirty = false; }

  private:
    AH::Array<VUState, BankSize> states = {{}};
    bool dirty = true;
    AH::Timer<millis> decayTimer;
};

} // namespace Bankable

} // namespace MCU

END_CS_NAMESPACE