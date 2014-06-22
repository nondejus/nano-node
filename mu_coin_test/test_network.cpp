#include <gtest/gtest.h>
#include <boost/thread.hpp>
#include <mu_coin/mu_coin.hpp>

TEST (network, construction)
{
    mu_coin::system system (24001, 1);
    ASSERT_EQ (1, system.clients.size ());
    ASSERT_EQ (24001, system.clients [0]->network.socket.local_endpoint ().port ());
}

TEST (network, send_keepalive)
{
    mu_coin::system system (24001, 2);
    system.clients [0]->network.receive ();
    system.clients [1]->network.receive ();
    system.clients [0]->network.send_keepalive (system.endpoint (1));
    while (system.clients [0]->network.keepalive_ack_count == 0)
    {
        system.service.run_one ();
    }
    auto peers1 (system.clients [0]->peers.list ());
    auto peers2 (system.clients [1]->peers.list ());
    ASSERT_EQ (1, system.clients [0]->network.keepalive_ack_count);
    ASSERT_NE (peers1.end (), std::find (peers1.begin (), peers1.end (), system.endpoint (1)));
    ASSERT_EQ (1, system.clients [1]->network.keepalive_req_count);
    ASSERT_NE (peers2.end (), std::find (peers2.begin (), peers2.end (), system.endpoint (0)));
}

TEST (network, publish_req)
{
    auto block (std::unique_ptr <mu_coin::send_block> (new mu_coin::send_block));
    mu_coin::keypair key1;
    mu_coin::keypair key2;
    block->hashables.previous = 0;
    block->hashables.balance = 200;
    block->hashables.destination = key2.pub;
    mu_coin::publish_req req (std::move (block));
    mu_coin::byte_write_stream stream;
    req.serialize (stream);
    mu_coin::publish_req req2;
    mu_coin::byte_read_stream stream2 (stream.data, stream.size);
    auto error (req2.deserialize (stream2));
    ASSERT_FALSE (error);
    ASSERT_EQ (*req.block, *req2.block);
}

TEST (network, send_discarded_publish)
{
    mu_coin::system system (24001, 2);
    std::unique_ptr <mu_coin::send_block> block (new mu_coin::send_block);
    system.clients [0]->network.publish_block (system.endpoint (1), std::move (block));
    while (system.clients [1]->network.publish_req_count == 0)
    {
        system.service.run_one ();
    }
    ASSERT_EQ (1, system.clients [1]->network.publish_req_count);
    ASSERT_EQ (0, system.clients [0]->network.publish_nak_count);
}

TEST (network, send_invalid_publish)
{
    mu_coin::system system (24001, 2);
    std::unique_ptr <mu_coin::send_block> block (new mu_coin::send_block);
    mu_coin::keypair key1;
    block->hashables.previous = 0;
    block->hashables.balance = 20;
    mu_coin::sign_message (key1.prv, key1.pub, block->hash (), block->signature);
    system.clients [0]->network.publish_block (system.endpoint (1), std::move (block));
    while (system.clients [0]->network.publish_unk_count == 0)
    {
        system.service.run_one ();
    }
    ASSERT_EQ (1, system.clients [1]->network.publish_req_count);
    ASSERT_EQ (1, system.clients [0]->network.publish_unk_count);
}

TEST (network, send_valid_publish)
{
    mu_coin::system system (24001, 2);
    mu_coin::secret_key secret;
    mu_coin::keypair key1;
    system.clients [0]->wallet.insert (key1.pub, key1.prv, secret);
    system.clients [0]->store.genesis_put (key1.pub, 100);
    mu_coin::keypair key2;
    system.clients [1]->wallet.insert (key2.pub, key2.prv, secret);
    system.clients [1]->store.genesis_put (key1.pub, 100);
    mu_coin::send_block block2;
    mu_coin::block_hash hash1;
    ASSERT_FALSE (system.clients [0]->store.latest_get (key1.pub, hash1));
    block2.hashables.previous = hash1;
    block2.hashables.balance = 50;
    block2.hashables.destination = key2.pub;
    auto hash2 (block2.hash ());
    mu_coin::sign_message (key1.prv, key1.pub, hash2, block2.signature);
    mu_coin::block_hash hash3;
    ASSERT_FALSE (system.clients [1]->store.latest_get (key1.pub, hash3));
    system.clients [0]->processor.publish (std::unique_ptr <mu_coin::block> (new mu_coin::send_block (block2)), system.endpoint (0));
    while (system.clients [0]->network.publish_con_count == 0)
    {
        system.service.run_one ();
    }
    ASSERT_EQ (1, system.clients [0]->network.publish_con_count);
    ASSERT_EQ (0, system.clients [1]->network.publish_con_count);
    ASSERT_EQ (0, system.clients [0]->network.publish_req_count);
    ASSERT_EQ (1, system.clients [1]->network.publish_req_count);
    mu_coin::block_hash hash4;
    ASSERT_FALSE (system.clients [1]->store.latest_get (key1.pub, hash4));
    ASSERT_FALSE (hash3 == hash4);
    ASSERT_EQ (hash2, hash4);
    ASSERT_EQ (50, system.clients [1]->ledger.balance (key1.pub));
}

TEST (receivable_processor, timeout)
{
    mu_coin::system system (24001, 1);
    auto receivable (std::make_shared <mu_coin::receivable_processor> (nullptr, *system.clients [0]));
    ASSERT_EQ (0, system.clients [0]->network.publish_listener_size ());
    ASSERT_FALSE (receivable->complete);
    ASSERT_EQ (0, system.processor.size ());
    receivable->advance_timeout ();
    ASSERT_EQ (1, system.processor.size ());
    receivable->advance_timeout ();
    ASSERT_EQ (2, system.processor.size ());
}

TEST (receivable_processor, confirm_no_pos)
{
    mu_coin::system system (24001, 1);
    auto block1 (new mu_coin::send_block ());
    auto receivable (std::make_shared <mu_coin::receivable_processor> (std::unique_ptr <mu_coin::publish_req> {new mu_coin::publish_req {std::unique_ptr <mu_coin::block> {block1}}}, *system.clients [0]));
    receivable->run ();
    ASSERT_EQ (1, system.clients [0]->network.publish_listener_size ());
    mu_coin::keypair key1;
    mu_coin::publish_con con1 {block1->hash ()};
    mu_coin::authorization auth1;
    auth1.address = key1.pub;
    mu_coin::sign_message (key1.prv, key1.pub, con1.block, auth1.signature);
    con1.authorizations.push_back (auth1);
    mu_coin::byte_write_stream stream;
    con1.serialize (stream);
    ASSERT_LE (stream.size, system.clients [0]->network.buffer.size ());
    std::copy (stream.data, stream.data + stream.size, system.clients [0]->network.buffer.begin ());
    system.clients [0]->network.receive_action (boost::system::error_code {}, stream.size);
    ASSERT_TRUE (receivable->acknowledged.is_zero ());
}

TEST (receivable_processor, confirm_insufficient_pos)
{
    mu_coin::system system (24001, 1);
    mu_coin::keypair key1;
    system.clients [0]->ledger.store.genesis_put (key1.pub, 1);
    auto block1 (new mu_coin::send_block ());
    auto receivable (std::make_shared <mu_coin::receivable_processor> (std::unique_ptr <mu_coin::publish_req> {new mu_coin::publish_req {std::unique_ptr <mu_coin::block> {block1}}}, *system.clients [0]));
    receivable->run ();
    ASSERT_EQ (1, system.clients [0]->network.publish_listener_size ());
    mu_coin::publish_con con1 {block1->hash ()};
    mu_coin::authorization auth1;
    auth1.address = key1.pub;
    mu_coin::sign_message (key1.prv, key1.pub, con1.block, auth1.signature);
    con1.authorizations.push_back (auth1);
    mu_coin::byte_write_stream stream;
    con1.serialize (stream);
    ASSERT_LE (stream.size, system.clients [0]->network.buffer.size ());
    std::copy (stream.data, stream.data + stream.size, system.clients [0]->network.buffer.begin ());
    system.clients [0]->network.receive_action (boost::system::error_code {}, stream.size);
    ASSERT_EQ (1, receivable->acknowledged);
    ASSERT_FALSE (receivable->complete);
    // Shared_from_this, local, timeout, callback
    ASSERT_EQ (4, receivable.use_count ());
}

TEST (receivable_processor, confirm_sufficient_pos)
{
    mu_coin::system system (24001, 1);
    mu_coin::keypair key1;
    system.clients [0]->ledger.store.genesis_put (key1.pub, std::numeric_limits<mu_coin::uint256_t>::max ());
    auto block1 (new mu_coin::send_block ());
    auto receivable (std::make_shared <mu_coin::receivable_processor> (std::unique_ptr <mu_coin::publish_req> {new mu_coin::publish_req {std::unique_ptr <mu_coin::block> {block1}}}, *system.clients [0]));
    receivable->run ();
    ASSERT_EQ (1, system.clients [0]->network.publish_listener_size ());
    mu_coin::publish_con con1 {block1->hash ()};
    mu_coin::authorization auth1;
    auth1.address = key1.pub;
    mu_coin::sign_message (key1.prv, key1.pub, con1.block, auth1.signature);
    con1.authorizations.push_back (auth1);
    mu_coin::byte_write_stream stream;
    con1.serialize (stream);
    ASSERT_LE (stream.size, system.clients [0]->network.buffer.size ());
    std::copy (stream.data, stream.data + stream.size, system.clients [0]->network.buffer.begin ());
    system.clients [0]->network.receive_action (boost::system::error_code {}, stream.size);
    ASSERT_EQ (std::numeric_limits<mu_coin::uint256_t>::max (), receivable->acknowledged);
    ASSERT_TRUE (receivable->complete);
    ASSERT_EQ (3, receivable.use_count ());
}

TEST (receivable_processor, send_with_receive)
{
    mu_coin::system system (24001, 2);
    mu_coin::keypair key1;
    system.clients [0]->wallet.insert (key1.pub, key1.prv, 0);
    mu_coin::keypair key2;
    system.clients [1]->wallet.insert (key2.pub, key2.prv, 0);
    auto amount (std::numeric_limits <mu_coin::uint256_t>::max ());
    system.clients [0]->ledger.store.genesis_put (key1.pub, amount);
    system.clients [1]->ledger.store.genesis_put (key1.pub, amount);
    auto block1 (new mu_coin::send_block ());
    mu_coin::block_hash previous;
    ASSERT_FALSE (system.clients [0]->ledger.store.latest_get (key1.pub, previous));
    block1->hashables.previous = previous;
    block1->hashables.balance = amount - 100;
    block1->hashables.destination = key2.pub;
    mu_coin::sign_message (key1.prv, key1.pub, block1->hash (), block1->signature);
    ASSERT_EQ (amount, system.clients [0]->ledger.balance (key1.pub));
    ASSERT_EQ (0, system.clients [0]->ledger.balance (key2.pub));
    ASSERT_EQ (amount, system.clients [1]->ledger.balance (key1.pub));
    ASSERT_EQ (0, system.clients [1]->ledger.balance (key2.pub));
    ASSERT_EQ (mu_coin::process_result::progress, system.clients [0]->ledger.process (*block1));
    ASSERT_EQ (mu_coin::process_result::progress, system.clients [1]->ledger.process (*block1));
    ASSERT_EQ (amount - 100, system.clients [0]->ledger.balance (key1.pub));
    ASSERT_EQ (0, system.clients [0]->ledger.balance (key2.pub));
    ASSERT_EQ (amount - 100, system.clients [1]->ledger.balance (key1.pub));
    ASSERT_EQ (0, system.clients [1]->ledger.balance (key2.pub));
    auto receivable (std::make_shared <mu_coin::receivable_processor> (std::unique_ptr <mu_coin::publish_req> {new mu_coin::publish_req {std::unique_ptr <mu_coin::block> {block1}}}, *system.clients [1]));
    receivable->run ();
    ASSERT_EQ (1, system.clients [1]->network.publish_listener_size ());
    while (!receivable->complete)
    {
        system.service.run_one ();
    }
    ASSERT_EQ (amount - 100, system.clients [0]->ledger.balance (key1.pub));
    ASSERT_EQ (0, system.clients [0]->ledger.balance (key2.pub));
    ASSERT_EQ (amount - 100, system.clients [1]->ledger.balance (key1.pub));
    ASSERT_EQ (100, system.clients [1]->ledger.balance (key2.pub));
    ASSERT_EQ (amount - 100, receivable->acknowledged);
    ASSERT_TRUE (receivable->complete);
    ASSERT_EQ (3, receivable.use_count ());
    while (system.clients [0]->network.publish_req_count < 1)
    {
        system.service.run_one ();
    }
    ASSERT_EQ (amount - 100, system.clients [0]->ledger.balance (key1.pub));
    ASSERT_EQ (100, system.clients [0]->ledger.balance (key2.pub));
    ASSERT_EQ (amount - 100, system.clients [1]->ledger.balance (key1.pub));
    ASSERT_EQ (100, system.clients [1]->ledger.balance (key2.pub));
    ASSERT_EQ (amount - 100, receivable->acknowledged);
}

TEST (client, send_single)
{
    boost::asio::io_service io_service;
    mu_coin::processor_service processor;
    mu_coin::client client1 (io_service, 24001, processor);
    mu_coin::keypair key1;
    mu_coin::keypair key2;
    mu_coin::uint256_union password1;
    client1.wallet.insert (key1.pub, key1.prv, password1);
    client1.wallet.insert (key2.pub, key2.prv, password1);
    client1.store.genesis_put (key1.pub, 100000);
    ASSERT_FALSE (client1.send (key2.pub, 1000, password1));
    ASSERT_EQ (100000 - 1000, client1.ledger.balance (key1.pub));
}