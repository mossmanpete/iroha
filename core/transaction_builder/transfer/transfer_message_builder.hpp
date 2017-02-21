/*
Copyright 2016 Soramitsu Co., Ltd.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef CORE_MODEL_TRANSACTION_BUILDER_TRANSFER_MESSAGE_HPP
#define CORE_MODEL_TRANSACTION_BUILDER_TRANSFER_MESSAGE_HPP

#include <infra/protobuf/api.pb.h>
#include "../transaction_builder_base.hpp"
#include "../type_signatures/commands/transfer.hpp"
#include "../type_signatures/objects/message.hpp"

namespace transaction {

template <>
class TransactionBuilder<type_signatures::Transfer<type_signatures::Message>> {
 public:
  TransactionBuilder() = default;
  TransactionBuilder(const TransactionBuilder&) = default;
  TransactionBuilder(TransactionBuilder&&) = default;

  TransactionBuilder& setSenderPublicKey(std::string sender) {
    if (_isSetSenderPublicKey) {
      throw std::domain_error(std::string("Duplicate sender in ") +
                              "transfer/transfer_message_builder_template.hpp");
    }
    _isSetSenderPublicKey = true;
    _senderPublicKey = std::move(sender);
    return *this;
  }

  TransactionBuilder& setMessage {
    if (_isSetMessage) {
      throw std::domain_error(std::string("Duplicate ") + "Message" + " in " +
                              "transfer/transfer_message_builder_template.hpp");
    }
    _isSetMessage = true;
    return *this;
  }

  transaction::Transaction build() {
    const auto unsetMembers = enumerateUnsetMembers();
    if (not unsetMembers.empty()) {
      throw exception::transaction::UnsetBuildArgmentsException(
          "Transfer<object::Message>", unsetMembers);
    }
    return transaction::Transaction(_sender, command::Transfer(_message));
  }

 private:
  std::string enumerateUnsetMembers() {
    std::string ret;
    if (not _isSetSenderPublicKey) ret += std::string(" ") + "sender";
    if (not _isSetMessage) ret += std::string(" ") + "Message";
    return ret;
  }

  std::string _senderPublicKey;
  Api::Message _message;

  bool _isSetSenderPublicKey = false;
  bool _isSetMessage = false;
};
}

#endif
