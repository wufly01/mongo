/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/request_builder_interface.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {
namespace rpc {

class OpMsgReply final : public rpc::ReplyInterface {
public:
    explicit OpMsgReply(OpMsg msg) : _msg(std::move(msg)) {}
    const BSONObj& getMetadata() const override {
        return _msg.body;
    }
    const BSONObj& getCommandReply() const override {
        return _msg.body;
    }
    rpc::Protocol getProtocol() const override {
        return rpc::Protocol::kOpMsg;
    }

private:
    OpMsg _msg;
};

class OpMsgReplyBuilder final : public rpc::ReplyBuilderInterface {
public:
    ReplyBuilderInterface& setRawCommandReply(const BSONObj& reply) override {
        _builder.beginBody().appendElements(reply);
        return *this;
    }
    BSONObjBuilder getInPlaceReplyBuilder(std::size_t reserveBytes) override {
        BSONObjBuilder bob = _builder.beginBody();
        // Eagerly reserve space and claim our reservation immediately so we can actually write data
        // to it.
        bob.bb().reserveBytes(reserveBytes);
        bob.bb().claimReservedBytes(reserveBytes);
        return bob;
    }
    ReplyBuilderInterface& setMetadata(const BSONObj& metadata) override {
        _builder.resumeBody().appendElements(metadata);
        return *this;
    }
    rpc::Protocol getProtocol() const override {
        return rpc::Protocol::kOpMsg;
    }
    void reset() override {
        _builder.reset();
    }
    Message done() override {
        return _builder.finish();
    }

private:
    OpMsgBuilder _builder;
};

class OpMsgRequest final : public rpc::RequestInterface {
public:
    explicit OpMsgRequest(mongo::OpMsgRequest msg) : _msg(std::move(msg)) {}
    StringData getDatabase() const override {
        return _msg.getDatabase();
    }
    StringData getCommandName() const override {
        return _msg.getCommandName();
    }
    const BSONObj& getMetadata() const override {
        return _msg.body;
    }
    const BSONObj& getCommandArgs() const override {
        return _msg.body;
    }
    rpc::Protocol getProtocol() const override {
        return rpc::Protocol::kOpMsg;
    }

private:
    const mongo::OpMsgRequest _msg;
};

class OpMsgRequestBuilder final : public rpc::RequestBuilderInterface {
public:
    RequestBuilderInterface& setDatabase(StringData database) override {
        _db = database.toString();
        return *this;
    }
    RequestBuilderInterface& setCommandName(StringData commandName) override {
        // No-op because command name is first field name in command body.
        return *this;
    }
    RequestBuilderInterface& setCommandArgs(BSONObj reply) override {
        _builder.beginBody().appendElements(reply);
        return *this;
    }
    RequestBuilderInterface& setMetadata(BSONObj metadata) override {
        _builder.resumeBody().appendElements(metadata);
        return *this;
    }
    rpc::Protocol getProtocol() const override {
        return rpc::Protocol::kOpMsg;
    }
    Message done() override {
        invariant(!_db.empty());
        _builder.resumeBody().append("$db", _db);
        return _builder.finish();
    }

private:
    std::string _db;
    OpMsgBuilder _builder;
};

}  // namespace rpc
}  // namespace mongo
