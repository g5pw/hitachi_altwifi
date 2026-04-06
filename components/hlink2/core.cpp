
#include "esphome/core/log.h"

#include "tools.h"
#include "message.h"
#include "mnemonics.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "core.h"
#include "esphome/components/api/custom_api_device.h"


namespace esphome {
namespace hlink2 {
static const char *const TAG = "hlink2:core";


Core::Core() {
  this->buffer_rx_.set_state(IOBuffer::State::EMPTY);
}


/* ------------------------- SETUP ------------------------- */
void Core::setup() {
  this->set_task(Core::Task::EXCHANGE);
  api_device_ = new api::CustomAPIDevice();
  this->api_device_->register_service(
    &Core::action_custom_message, "send_message",
    std::array<std::string, 3>{{"flag", "items", "qos"}}
  );
}

void Core::dump_config() {
  ESP_LOGCONFIG(TAG, "hlink2 Core");
  ESP_LOGCONFIG(TAG, "Ephemeral Messages: %zu/%zu", this->ephemeral_messages_.size(), this->ephemeral_messages_.capacity());
  ESP_LOGCONFIG(TAG, "Persistent Messages: %zu/%zu", this->persistent_messages_.size(), this->persistent_messages_.capacity());

  for (size_t i=0; i < this->persistent_messages_.size(); i++) {
    ESP_LOGCONFIG(TAG, " %s", this->persistent_messages_[i].config().c_str());
  }
}


template<typename TYPE, Mnemonic::Mode MODE, Mnemonic::Dest DEST>
Message* Core::register_message(MessageParameters params){
  constexpr const Message::Flag& flag = *Message::Flag::flag(MODE, DEST);

  TYPE* vector{nullptr};
  if constexpr (std::is_same_v<TYPE, Core::EphemeralVector>) {
    vector = &this->ephemeral_messages_;
    params.ephemeral = true;
  } else if constexpr (std::is_same_v<TYPE, Core::PersistentVector>) {
    vector = &this->persistent_messages_;
    params.ephemeral = false;
  } else {
    static_assert(std::is_same_v<TYPE, Core::EphemeralVector> || std::is_same_v<TYPE, Core::PersistentVector>, "register_message: bad TYPE");
  }

  if (vector->full()) {
    ESP_LOGE(TAG, "register_message: vector full");
    return nullptr;
  }

  if constexpr (MODE == Mnemonic::Mode::STS) {
    params.update_interval = params.update_interval.value_or(MESSAGE_UPDATE_INTERVAL);
    params.qos = params.qos.value_or(static_cast<Message::QoS>(CONF_MESSAGE_STS_QOS));
  } else if constexpr (MODE == Mnemonic::Mode::CMD) {
    params.update_interval = 0;
    params.qos = params.qos.value_or(static_cast<Message::QoS>(CONF_MESSAGE_CMD_QOS));
    uint32_t buzzer = params.buzzer.value_or(MESSAGE_BUZZER);
  } else {
    static_assert(MODE == Mnemonic::Mode::STS || MODE == Mnemonic::Mode::CMD, "register_message: bad MODE");
  }

  if (vector->emplace_back(flag, *params.qos, *params.update_interval, *params.ephemeral)) {
    Message& msg = vector->back();
    this->messages_.add(&msg);
    ESP_LOGD(TAG, "New %s message @%p", flag.raw, &msg);
    msg.initialize();
    if constexpr (MODE == Mnemonic::Mode::STS) {
      msg.interval().restart();
    }
    if constexpr (MODE == Mnemonic::Mode::CMD) {
      if (*params.buzzer) msg.add(Mnemonic::IDU_Buzz);
    }
    return &msg;
  }
  ESP_LOGE(TAG, "register_message: failed to add message (size=%zu)", vector->size());
  return nullptr;
}

// explicit declarations of register_message
template Message *Core::register_message<Core::PersistentVector, Mnemonic::Mode::CMD, Mnemonic::Dest::IDU>(MessageParameters);
template Message *Core::register_message<Core::PersistentVector, Mnemonic::Mode::CMD, Mnemonic::Dest::ODU>(MessageParameters);
template Message *Core::register_message<Core::PersistentVector, Mnemonic::Mode::STS, Mnemonic::Dest::IDU>(MessageParameters);
template Message *Core::register_message<Core::PersistentVector, Mnemonic::Mode::STS, Mnemonic::Dest::ODU>(MessageParameters);
template Message *Core::register_message<Core::EphemeralVector, Mnemonic::Mode::CMD, Mnemonic::Dest::IDU>(MessageParameters);
template Message *Core::register_message<Core::EphemeralVector, Mnemonic::Mode::CMD, Mnemonic::Dest::ODU>(MessageParameters);
template Message *Core::register_message<Core::EphemeralVector, Mnemonic::Mode::STS, Mnemonic::Dest::IDU>(MessageParameters);
template Message *Core::register_message<Core::EphemeralVector, Mnemonic::Mode::STS, Mnemonic::Dest::ODU>(MessageParameters);


Message* Core::register_status_mnemonic(Mnemonic& m, MessageParameters params, bool promote) {
  if (!m.match_mode(Mnemonic::Mode::STS)) {
    ESP_LOGE(TAG, "register_status_mnemonic: %s: invalid mode (%lu)", m.name, m.mode());
    return nullptr;
  }
  params.update_interval = params.update_interval.value_or(MESSAGE_UPDATE_INTERVAL);
  params.qos = params.qos.value_or(Message::QoS::QOS_LOW);

  if (promote) {
    for (size_t i=0; i< this->persistent_messages_.size(); i++) {
      if (
        this->persistent_messages_[i].flag->match_mode(Mnemonic::Mode::STS)
        && this->persistent_messages_[i].flag->match_dest(m.dest)
        && this->persistent_messages_[i].interval().delay() <= *params.update_interval
        && this->persistent_messages_[i].qos >= *params.qos
      ){
        // promote if already in message
        if (this->persistent_messages_[i].has(m)) {
          return &this->persistent_messages_[i];
        };
        if (
          this->persistent_messages_[i].interval().delay() == *params.update_interval
          && this->persistent_messages_[i].add(m)
        ) {
          return &this->persistent_messages_[i];
        }
      }
    }
  }
  Message* msg;
  switch (m.dest) {
    case Mnemonic::Dest::IDU:
      msg = this->register_message<Core::PersistentVector, Mnemonic::Mode::STS, Mnemonic::Dest::IDU>(params);
      break;
    case Mnemonic::Dest::ODU:
      msg = this->register_message<Core::PersistentVector, Mnemonic::Mode::STS, Mnemonic::Dest::ODU>(params);
      break;
    default: return nullptr;
  }
  if(msg) {
    if (msg->add(m)) {
      return msg;
    } else {
      ESP_LOGE(TAG, "register_status_mnemonic: failed to add mnemonic %s", m.name);
      this->messages_.pop_back(msg);
    }
  }
  return nullptr;
}

template<Mnemonic::Mode MODE>
Message* Core::register_custom_message(Mnemonic::Dest dest, std::vector<std::string> items, MessageParameters params) {
  Message* msg{nullptr};

  switch (dest) {
    case Mnemonic::Dest::IDU:
      msg = this->register_ephemeral_message<MODE, Mnemonic::Dest::IDU>(params);
      break;
    case Mnemonic::Dest::ODU:
      msg = this->register_ephemeral_message<MODE, Mnemonic::Dest::ODU>(params);
      break;
    default:
      ESP_LOGE(TAG, "register_custom_message: invalid dest %u", dest);
      return nullptr;
  }
  if (msg) {
    Mnemonic* mn;
    std::string_view name;
    std::string_view value;
    for (std::string item: items) {
      if constexpr (MODE == Mnemonic::Mode::CMD) {
        const char* split = item.c_str();
        while (*split && *split != ':') split++;
        if (*split == '\0') {
          ESP_LOGW(TAG, "register_custom_message: missing ':' (%s)", item);
          continue;
        }
        name = std::string_view(item.c_str(), split - item.c_str());
        value = std::string_view(split + 1);
      } else {
        name = std::string_view(item);
      }

      mn = Mnemonic::from_hash(name, dest);
      if (mn == &Mnemonic::None) {
        ESP_LOGW(TAG, "register_custom_message: unknow mnemonic %s_%.*s", Mnemonic::DestToString(dest), name.size(), name.data());
        continue;
      }
      if (auto request = msg->add(*mn)) {
        if constexpr (MODE == Mnemonic::Mode::CMD) {
          if (request->set_request(value, true) ) {
          } else {
            ESP_LOGE(TAG, "register_custom_message: failed to add request (%.*s)", value.size(), value.data());
          }
        }
      } else {
        ESP_LOGW(TAG, "register_custom_message: failed to add mnemonic %s_%.*s",  Mnemonic::DestToString(dest), name.size(), name.data());
      }
    }
    if (msg->empty()) {
      ESP_LOGW(TAG, "register_custom_message: empty message");
      msg->set_state(Message::State::INVALID); // garbage collection
      return nullptr;
    }
    this->schedule_message(msg);
    ESP_LOGI(TAG, "register_custom_message: registered %s", msg->config().c_str());
    return msg;
  }
  ESP_LOGE(TAG, "register_custom_message: registration failed");
  return nullptr;
}


void Core::action_custom_message(std::string flag, std::vector<std::string> items, int32_t qos) {
  const Message::Flag* f = Message::Flag::from_raw(flag);
  Message* msg;
  switch (f->mode) {
    case Mnemonic::Mode::STS:
      msg = register_custom_message<Mnemonic::Mode::STS>(f->dest, items, MessageParameters{.qos = static_cast<Message::QoS>(qos), .ephemeral=true});
      break;
    case Mnemonic::Mode::CMD:
      msg = register_custom_message<Mnemonic::Mode::CMD>(f->dest, items, MessageParameters{.qos = static_cast<Message::QoS>(qos), .buzzer=true, .ephemeral=true});
      break;
    default:
      ESP_LOGE(TAG, "action_custom_message: invalid mode (flag %s -> %s)", flag.c_str(), f->raw);
      return;
  }      
}

void Core::schedule_message(Message* msg) {
  msg->reinitialize();
  msg->schedule();
}

void Core::force_update_messages() {
  for (size_t index = 0; index < this->persistent_messages_.size(); ++index) {
    if (this->persistent_messages_[index].interval().delay() && this->persistent_messages_[index].state() == Message::State::INITIALIZED) {
      this->persistent_messages_[index].interval().force_expiration();
    }
  }
}

/* ------------------------- STATE MACHINE ------------------------- */

void Core::clear_current_message() {
  if (
    this->current_message_->flag->match_mode(Mnemonic::Mode::CMD)
    && this->current_message_->state() > Message::State::PARSED
  ) {
    this->current_message_->clear_requests();
  }
  this->current_message_ = nullptr;
}

void Core::loop() {
  Timer timeout{TASK_TIMEOUT};
  bool switch_task = false;

  do {
    switch (this->current_task_) {
      case Task::EXCHANGE: {
        if (!this->current_message_ && !this->task_schedule()) {
          switch_task = true;
          break;
        }
        switch (Message::State current_state = this->current_message_->state() ) {
          case Message::State::SCHEDULED: 
            this->task_format();
            break;
          case Message::State::FORMATTED:
            this->task_transmit();
            break;
          case Message::State::SENT:
            this->task_receive();
            break;
          case Message::State::RECEIVED:
            this->task_parse();
            break;
          case Message::State::PARSED:
            this->clear_current_message();
            this->task_process();
            switch_task = true;
            break;
          case Message::State::EXPIRED:
            ESP_LOGW(TAG, "loop: @%p expired (%s)", this->current_message_, this->current_message_->config().c_str());
            this->current_message_->reinitialize();       
            this->clear_current_message();
            switch_task = true;
            break;
          default:
            ESP_LOGE(TAG, "loop: @%p: discarded, state %s", this->current_message_, Message::StateName[static_cast<uint8_t>(current_state)]);
            this->clear_current_message();
            switch_task = true;
            break;
        }
      }
      case Task::PROCESS: {
        if (!this->received_messages.size()) {
          switch_task = true;
          break;
        }
        this->task_process();
      }
      default: switch_task = true;
      if (switch_task) this->set_task(this->next_task());
    }
  } while (!timeout.expired());
  this->set_task(this->next_task());
}

/* ------------------------- TASKS ------------------------- */

bool Core::task_schedule() {
  ESP_LOGV(TAG, "SCHEDULE: start");

  if ((this->current_message_ = this->messages_.select(
      [this](Message* msg) {
        switch (msg->state()) {
          case Message::State::PROCESSED:
          case Message::State::EXPIRED:
          case Message::State::INVALID:
            if (msg->ephemeral() && msg == &this->ephemeral_messages_.front()) { // remove first ephemeral message, which is the most probable (other will wait)
              ESP_LOGD(TAG, "@%p removed from ephemeral messages (%s)", msg, Message::StateName[static_cast<uint8_t>(msg->state())]);
              this->ephemeral_messages_.pop_front();
              return -1; // remove msg (garbage collection)
            }
            msg->reinitialize(); // no break to exec initialized instructions
          case Message::State::INITIALIZED:
            if (!msg->interval().expired()) return 0; // check is fast if delay==0 because expired() evaluate delay first
            msg->schedule();
            msg->interval().restart();
          case Message::State::SCHEDULED:
            return 1;
          default: return 0;
        }
      }
    )
  )) {
    ESP_LOGD(TAG, "SCHEDULE: @%p %s", this->current_message_, this->current_message_->flag->raw);
  }

  ESP_LOGV(TAG, "SCHEDULE: no message to schedule");
  return false;
}


void Core::task_format() {
  ESP_LOGV(TAG, "FORMAT: start");
  if (this->current_message_->flag->match_mode(Mnemonic::Mode::CMD)) {
    if (auto request = this->current_message_->get_request(Mnemonic::IDU_Buzz)) {
      request->set_request(this->buzzer() ? '1' : '0');
    }
  }
  switch (this->current_message_->format()) { 
    case Message::State::FORMATTED:
      ESP_LOGV(TAG, "FORMAT: @%p: done", this->current_message_);
      return;
    case Message::State::EXPIRED:
      ESP_LOGW(TAG, "FORMAT: expired");
      return;
    default:
      ESP_LOGE(TAG, "FORMAT: failed");
      return;
  }
}


void Core::task_transmit() {
  IOBuffer* buffer = this->current_message_->request_buffer();
  Timer timeout{TASK_TIMEOUT};
  ESP_LOGV(TAG, "TRANSMIT: start, %zu/%zu bytes", buffer->view_index(), buffer->size());
  
  // empty read buffer (there is no way to know to which message attribute read data)
  size_t available_bytes;
  uint8_t buffer_[256];
  while ((available_bytes = this->available()) && !timeout.expired()) {
    this->read_array(buffer_, std::min(sizeof(buffer_), available_bytes));
    ESP_LOGW(TAG, "TRANSMIT: discarded RX buffer (%zu), %s", available_bytes, debug_data(buffer_, available_bytes).c_str());
  }

  // transmit TX buffer
  size_t len;
  while ((len = std::min<size_t>(UART_TX_CHUNK_SIZE, buffer->view.size()))){
    this->write_array(reinterpret_cast<const uint8_t*>(buffer->view.data()), len);
    buffer->view.remove_prefix(len);
    if (timeout.expired()) {
      ESP_LOGV(TAG, "TRANSMIT: timeout, %d/%d bytes", buffer->view_index(), buffer->size());
      return;
    };
  }

  this->current_message_->set_state(Message::State::SENT);
  this->buffer_rx_.set_state(IOBuffer::State::EMPTY);
  ESP_LOGV(TAG, "TRANSMIT: @%p: done, %d/%d bytes", this->current_message_, buffer->view_index(), buffer->size());
  return;
}


void Core::task_receive() {
  Timer timeout{TASK_TIMEOUT};
  ESP_LOGV(TAG, "RECEIVE: start, %d bytes", this->buffer_rx_.size());
  
  uint8_t data;
  int available_bytes;
  size_t len;
  while (this->buffer_rx_.free()) {
    if (timeout.expired()) {
      ESP_LOGV(TAG, "RECEIVE: timeout (%d bytes)", this->buffer_rx_.size());
      return;
    }

    if (int len = this->available()) {
      if (len > this->buffer_rx_.free()) {
        len = this->buffer_rx_.free();
        ESP_LOGE(TAG, "RECEIVE: buffer_rx too small, truncating at %u bytes", len);
      }
      ESP_LOGV(TAG, "RECEIVE: %d bytes to read", len);
      if (this->read_array(reinterpret_cast<uint8_t*>(this->buffer_rx_.next()), len)) {
        this->buffer_rx_.set_size(this->buffer_rx_.size() + len);
        this->buffer_rx_.update(); // update state and view
      } else {
        ESP_LOGE(TAG, "RECEIVE: failed to read");
      }

      switch (this->buffer_rx_.state()) {
        case IOBuffer::State::READY:
          ESP_LOGV(TAG, "RECEIVE: @%p: done (%d bytes)", this->current_message_, this->buffer_rx_.size());
          this->current_message_->set_state(Message::State::RECEIVED);
          return;
        case IOBuffer::State::INVALID:
          if (this->current_message_->set_state(Message::State::INVALID) == Message::State::EXPIRED) {
            ESP_LOGE(TAG, "RECEIVE: expired");
            return;
          } else {
            this->current_message_->set_state(Message::State::FORMATTED);
            ESP_LOGW(TAG, "RECEIVE: failed, retransmitting");
            return;
          }
        default:
          continue; // return;
        // unfinshed reception
        /// TODO: check whether ealy return is required (set but seems irrelevant)
      }
    }
  }
}


void Core::task_parse() {
  ESP_LOGV(TAG, "PARSE: start");
  switch(this->current_message_->parse(this->buffer_rx_)) {
    case Message::State::PARSED:
      ESP_LOGV(TAG, "PARSE: done, %d mnemonics to process", this->current_message_->response_mnemonics.size());

      if (
        this->current_message_->flag->match_mode(Mnemonic::Mode::CMD)
        && *this->current_message_->response == Message::Response::VALID
        && this->current_message_->size() > 0
      ){
        ESP_LOGV(TAG, "PARSE: @%p: use command values", this->current_message_);
        this->current_message_->apply_requests();
      }

      if (this->current_message_->response_mnemonics.empty()) {
        ESP_LOGI(TAG, "PARSE: @%p: drop (empty)", this->current_message_);
        this->current_message_->set_state(Message::State::PROCESSED);
        this->clear_current_message();
        return;
      }
      this->received_messages.push_back(this->current_message_);
      this->clear_current_message();
      return;
    case Message::State::EXPIRED:
      ESP_LOGW(TAG, "PARSE: expired");
      return;
    default:
      this->current_message_->set_state(Message::State::FORMATTED);
      ESP_LOGW(TAG, "PARSE: failed, retransmitting");
      return;
  }
}


void Core::task_process() {
  Timer timeout{TASK_TIMEOUT};
  ESP_LOGV(TAG, "PROCESS: start, %d messages to process", this->received_messages.size());
  do {
    Message* msg = this->received_messages.front();

    if (!msg->response) {
      ESP_LOGW(TAG, "PROCESS: @%p (%s) dropped, no response", msg, msg->flag->raw);
      this->received_messages.pop_front();
      continue;
    }
    switch (*msg->response) {
      case Message::Response::VALID:
        ESP_LOGV(TAG, "PROCESS: @%p: response VALID");
        break;
      case Message::Response::OTHER:
        ESP_LOGV(TAG, "PROCESS: @%p: response OTHER");
        *msg->response = Message::Response::VALID;
        break;
      case Message::Response::PARTIAL:
        ESP_LOGW(TAG, "PROCESS: @%p: response PARTIAL");
        break;
      case Message::Response::INVALID:
        ESP_LOGW(TAG, "PROCESS: @%p: response INVALID");
        this->received_messages.pop_front();
        continue;
      default:
        ESP_LOGW(TAG, "PROCESS: @%p: dropped, response unknown (%u)", msg, *msg->response);
        this->received_messages.pop_front();
        continue;
    }

    ESP_LOGV(TAG, "PROCESS: @%p: %u mnemonics to process", msg, msg->response_mnemonics.size());
    while (true) {
      Mnemonic* mnemonic = msg->response_mnemonics.front();
      ESP_LOGVV(TAG, "PROCESS: @%p: processing %s", msg, mnemonic->name);
      mnemonic->run_callbacks();
      msg->response_mnemonics.pop_front();
      ESP_LOGVV(TAG, "PROCESS: @%p: %s done", msg, mnemonic->name);
      if (msg->response_mnemonics.empty()) {
        // kept inside while to avoid useless timeout tests at the beginnng and at the end of the message
        break;
      } else if (timeout.expired()) {  
        ESP_LOGV(TAG, "PROCESS: @%p: timeout, %u remaining", msg, msg->response_mnemonics.size());
        return;
      }
    }
    
    ESP_LOGV(TAG, "PROCESS: @%p: done", msg);
    msg->run_callbacks();
    msg->set_state(Message::State::PROCESSED);
    if (msg->flag->match_mode(Mnemonic::Mode::CMD)) {
      msg->clear_requests();
    }
    this->received_messages.pop_front();

  } while (this->received_messages.size() > 0);
  return;
}

} // hlink2
} //esphome