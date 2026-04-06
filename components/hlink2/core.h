#pragma once

#include <cstdint>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"


#include "main.h"
#include "tools.h"
#include "buffer.h"
#include "message.h"
#include "mnemonics.h"

namespace esphome {
namespace api {
  class CustomAPIDevice;
}
}


namespace esphome {
  namespace api {class CustomAPIDevice; } // forward declaration
namespace hlink2 {


/// Message parameters
struct MessageParameters {
  std::optional<uint32_t> update_interval{std::nullopt};
  std::optional<Message::QoS> qos{};
  std::optional<bool> buzzer{};
  std::optional<bool> ephemeral{};
};


class Core: public uart::UARTDevice, public Component {
  public:
    using PersistentVector = RingVector<Message, MESSAGE_PERSISTENT_VECTOR_SIZE>;
    using EphemeralVector = RingVector<Message, MESSAGE_EPHEMERAL_VECTOR_SIZE>;

    enum class Task: uint8_t {
      EXCHANGE = 0,
      PROCESS = 1,
      size = 2,
      None = 0xFF
    };

    Core();

    /// override Component setup
    void setup() override;

    /// override Component loop
    void loop() override;

    /// override Component dump_config
    void dump_config() override;

    /// set Core task
    inline void set_task(const Task &task) { this->current_task_ = task;}

    /// return next Core task
    inline Task next_task() {
      return static_cast<Task>((static_cast<uint8_t>(this->current_task_) + 1 ) % static_cast<uint8_t>(Task::size));
    }
    
    /// return current Core task
    inline Task task() { return this->current_task_; }

    /// force update of all status messages, disregarding their update interval
    void force_update_messages();

    /// register a message (either persistent or ephemera)
    template<typename TYPE, Mnemonic::Mode, Mnemonic::Dest>
    Message* register_message(MessageParameters = {});

    /// register a persistent message (helper method)
    template<Mnemonic::Mode MODE, Mnemonic::Dest DEST>
    inline Message* register_persistent_message(MessageParameters params = {}) {
      return register_message<PersistentVector, MODE, DEST>(params); 
    }

    /// register an ephemeral message (helper method)
    template<Mnemonic::Mode MODE, Mnemonic::Dest DEST>
    inline Message* register_ephemeral_message(MessageParameters params = {}) {
      return register_message<EphemeralVector, MODE, DEST>(params);
    }

    /// register a persistent status mnemonic, registering a message if necessary (helper function)
    Message* register_status_mnemonic(Mnemonic&, MessageParameters = {}, bool promote = true);

    /// register an ephemeral custom message with a list of "mnemonics:values"
    template<Mnemonic::Mode MODE>
    Message* register_custom_message(Mnemonic::Dest, std::vector<std::string>, MessageParameters = {});

    /// api action callback to register an ephemeral custom message 
    void action_custom_message(std::string flag, std::vector<std::string> items, int32_t qos = 1);

    /// schedule a message
    void schedule_message(Message*);

    /// return buzzer state for command messages
    inline const bool buzzer() { return this->buzzer_; }
    inline void buzzer(const bool state) { this->buzzer_ = state; }

  protected:
    bool buzzer_{false};
    Task current_task_{Task::EXCHANGE};
    IOBuffer buffer_rx_{};
    api::CustomAPIDevice* api_device_{nullptr};
    PersistentVector persistent_messages_{};
    EphemeralVector ephemeral_messages_{};
    OrderedVector<Message*, PersistentVector::capacity() + EphemeralVector::capacity()> messages_;
    RingVector<Message*, MESSAGE_RECEIVED_VECTOR_SIZE> received_messages;
    Message* current_message_{nullptr};

    void clear_current_message();
    bool task_schedule();
    void task_format();
    void task_transmit();
    void task_receive();
    void task_parse();
    void task_process();
};


} // hlink2
} //esphome