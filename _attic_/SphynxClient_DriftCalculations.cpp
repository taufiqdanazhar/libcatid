/*
	Copyright (c) 2009-2010 Christopher A. Taylor.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of LibCat nor the names of its contributors may be used
	  to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

#include <cat/net/SphynxClient.hpp>
#include <cat/port/AlignedAlloc.hpp>
#include <cat/io/Logging.hpp>
#include <cat/io/MMapFile.hpp>
#include <cat/net/DNSClient.hpp>
#include <cat/io/Settings.hpp>
#include <cat/parse/BufferStream.hpp>
#include <cat/time/Clock.hpp>
#include <cat/crypt/SecureEqual.hpp>
#include <fstream>
using namespace std;
using namespace cat;
using namespace sphynx;


//// Client

Client::Client()
	: UDPEndpoint(REFOBJ_PRIO_0+4)
{
	_connected = false;
	_last_send_mstsc = 0;
	_destroyed = 0;

	// Clock synchronization
	_ts_base = 0;
	_ts_next_index = 0;
	_ts_B0 = 0.;
	_ts_B1 = 0;
	_ts_sample_count = 0;
}

Client::~Client()
{
	_kill_flag.Set();

	if (!WaitForThread(CLIENT_THREAD_KILL_TIMEOUT))
		AbortThread();
}

bool Client::SetServerKey(ThreadPoolLocalStorage *tls, const void *server_key, int key_bytes, const char *session_key)
{
	// Verify the key bytes are correct
	if (key_bytes != sizeof(_server_public_key))
	{
		WARN("Client") << "Failed to connect: Invalid server public key length provided";
		return false;
	}

	// Verify TLS is valid
	if (!tls->Valid())
	{
		WARN("Client") << "Failed to connect: Unable to create thread local storage";
		return false;
	}

	// Verify public key and initialize crypto library with it
	if (!_key_agreement_initiator.Initialize(tls->math, reinterpret_cast<const u8*>( server_key ), key_bytes))
	{
		WARN("Client") << "Failed to connect: Invalid server public key provided";
		return false;
	}

	// Generate a challenge for the server
	if (!_key_agreement_initiator.GenerateChallenge(tls->math, tls->csprng, _cached_challenge, CHALLENGE_BYTES))
	{
		WARN("Client") << "Failed to connect: Cannot generate challenge message";
		return false;
	}

	// Copy session key
	CAT_STRNCPY(_session_key, session_key, SESSION_KEY_BYTES);

	memcpy(_server_public_key, server_key, sizeof(_server_public_key));

	return true;
}

bool Client::Connect(const char *hostname, Port port)
{
	// Set port
	_server_addr.SetPort(port);

	// If DNS resolution fails,
	if (!DNSClient::ii->Resolve(hostname, fastdelegate::MakeDelegate(this, &Client::OnResolve), this))
	{
		WARN("Client") << "Failed to connect: Unable to resolve server hostname";
		return false;
	}

	return true;
}

bool Client::Connect(const NetAddr &addr)
{
	// Validate port
	if (addr.GetPort() == 0)
	{
		WARN("Client") << "Failed to connect: Invalid server port specified";
		return false;
	}

	_server_addr = addr;

	// Get SupportIPv6 flag from settings
	bool only_ipv4 = Settings::ii->getInt("Sphynx.Client.SupportIPv6", 0) == 0;

	// Get kernel receive buffer size
	int kernelReceiveBufferBytes = Settings::ii->getInt("Sphynx.Client.KernelReceiveBuffer", 1000000);

	// Attempt to bind to any port and accept ICMP errors initially
	if (!Bind(only_ipv4, 0, false, kernelReceiveBufferBytes))
	{
		WARN("Client") << "Failed to connect: Unable to bind to any port";
		return false;
	}

	// Convert server address if needed
	if (!_server_addr.Convert(Is6()))
	{
		WARN("Client") << "Failed to connect: Invalid address specified";
		Close();
		return false;
	}

	// Initialize max payload bytes
	InitializePayloadBytes(Is6());

	// Attempt to post hello message
	if (!PostHello())
	{
		WARN("Client") << "Failed to connect: Post failure";
		Close();
		return false;
	}

	// Attempt to start the timer thread
	if (!StartThread())
	{
		WARN("Client") << "Failed to connect: Unable to start timer thread";
		Close();
		return false;
	}

	return true;
}

bool Client::OnResolve(const char *hostname, const NetAddr *array, int array_length)
{
	// If resolve failed,
	if (array_length <= 0)
	{
		WARN("Client") << "Failed to connect: Server hostname resolve failed";

		Close();
	}
	else
	{
		NetAddr addr = array[0];
		addr.SetPort(_server_addr.GetPort());

		INANE("Client") << "Connecting: Resolved '" << hostname << "' to " << addr.IPToString();

		if (!Connect(addr))
			Close();
	}

	return true;
}

void Client::OnUnreachable(const NetAddr &src)
{
	// If IP matches the server and we're not connected yet,
	if (!_connected && _server_addr.EqualsIPOnly(src))
	{
		WARN("Client") << "Failed to connect: ICMP error received from server address";

		ConnectFail(ERR_CLIENT_ICMP);
	}
}

void Client::OnRead(ThreadPoolLocalStorage *tls, const NetAddr &src, u8 *data, u32 bytes)
{
	// If packet source is not the server, ignore this packet
	if (_server_addr != src)
		return;

	// If connection has completed
	if (_connected)
	{
		u32 buf_bytes = bytes;

		// If the data could not be decrypted, ignore this packet
		if (_auth_enc.Decrypt(data, buf_bytes))
		{
			_last_recv_tsc = Clock::msec_fast();

			// Pass the packet to the transport layer
			OnDatagram(tls, data, buf_bytes);
		}
		else
		{
			WARN("Client") << "Ignored invalid encrypted data";
		}
	}
	else if (bytes == S2C_COOKIE_LEN && data[0] == S2C_COOKIE)
	{
		// Allocate a post buffer
		u8 *pkt = AsyncBuffer::Acquire(C2S_CHALLENGE_LEN);

		if (!pkt)
		{
			WARN("Client") << "Unable to connect: Cannot allocate buffer for challenge message";

			ConnectFail(ERR_CLIENT_OUT_OF_MEMORY);

			return;
		}

		// Start ignoring ICMP unreachable messages now that we've seen a pkt from the server
		if (!IgnoreUnreachable())
		{
			WARN("Client") << "ICMP ignore unreachable failed";
		}

		// Construct challenge
		pkt[0] = C2S_CHALLENGE;

		u32 *magic = reinterpret_cast<u32*>( pkt + 1 );
		*magic = getLE32(PROTOCOL_MAGIC);

		u32 *in_cookie = reinterpret_cast<u32*>( data + 1 );
		u32 *out_cookie = reinterpret_cast<u32*>( pkt + 1 + 4 );
		*out_cookie = *in_cookie;

		memcpy(pkt + 1 + 4 + 4, _cached_challenge, CHALLENGE_BYTES);

		// Attempt to post a pkt
		if (!Post(_server_addr, pkt, C2S_CHALLENGE_LEN))
		{
			WARN("Client") << "Unable to connect: Cannot post pkt to cookie";

			ConnectFail(ERR_CLIENT_BROKEN_PIPE);
		}
		else
		{
			INANE("Client") << "Accepted cookie and posted challenge";
		}
	}
	else if (bytes == S2C_ANSWER_LEN && data[0] == S2C_ANSWER)
	{
		Port *port = reinterpret_cast<Port*>( data + 1 );

		Port server_session_port = getLE(*port);

		// Ignore packet if the port doesn't make sense
		if (server_session_port > _server_addr.GetPort())
		{
			Skein key_hash;

			// Process answer from server, ignore invalid
			if (_key_agreement_initiator.ProcessAnswer(tls->math, data + 1 + 2, ANSWER_BYTES, &key_hash) &&
				_key_agreement_initiator.KeyEncryption(&key_hash, &_auth_enc, _session_key))
			{
				_connected = true;

				// Note: Will now only listen to packets from the session port
				_server_addr.SetPort(server_session_port);

				OnConnect(tls);
			}
			else
			{
				INANE("Client") << "Ignored invalid server answer";
			}
		}
		else
		{
			INANE("Client") << "Ignored server answer with insane port";
		}
	}
	else if (bytes == S2C_ERROR_LEN && data[0] == S2C_ERROR)
	{
		HandshakeError err = (HandshakeError)data[1];

		if (err <= ERR_NUM_CLIENT_ERRORS)
		{
			INANE("Client") << "Ignored invalid server error";
			return;
		}

		WARN("Client") << "Unable to connect: Server returned error " << GetHandshakeErrorString(err);

		ConnectFail(err);
	}
}

bool Client::PostHello()
{
	if (_connected)
	{
		WARN("Client") << "Refusing to post hello after connected";
		return false;
	}

	// Allocate space for a post buffer
	u8 *pkt = AsyncBuffer::Acquire(C2S_HELLO_LEN);

	// If unable to allocate,
	if (!pkt)
	{
		WARN("Client") << "Cannot allocate a post buffer for hello packet";
		return false;
	}

	// Construct packet
	pkt[0] = C2S_HELLO;

	u32 *magic = reinterpret_cast<u32*>( pkt + 1 );
	*magic = getLE32(PROTOCOL_MAGIC);

	u8 *public_key = pkt + 1 + 4;
	memcpy(public_key, _server_public_key, PUBLIC_KEY_BYTES);

	// Attempt to post packet
	if (!Post(_server_addr, pkt, C2S_HELLO_LEN))
	{
		WARN("Client") << "Unable to post hello packet";
		return false;
	}

	INANE("Client") << "Posted hello packet";

	return true;
}

bool Client::PostTimePing()
{
	u32 timestamp = Clock::msec();

	// Write it out-of-band to avoid delays in transmission
	return WriteUnreliableOOB(IOP_C2S_TIME_PING, &timestamp, sizeof(timestamp), SOP_INTERNAL);
}

bool Client::ThreadFunction(void *)
{
	ThreadPoolLocalStorage tls;

	if (!tls.Valid())
	{
		WARN("Client") << "Unable to create thread pool local storage";

		return false;
	}

	u32 start_time = Clock::msec_fast();
	u32 first_hello_post = start_time;
	u32 last_hello_post = start_time;
	u32 hello_post_interval = INITIAL_HELLO_POST_INTERVAL;

	// While still not connected,
	while (!_connected)
	{
		// Wait for quit signal
		if (_kill_flag.Wait(HANDSHAKE_TICK_RATE))
			return false;

		// If now connected, break out
		if (_connected)
			break;

		u32 now = Clock::msec_fast();

		// If connection timed out,
		if (now - first_hello_post >= CONNECT_TIMEOUT)
		{
			// NOTE: Connection can complete before or after OnConnectFail()
			WARN("Client") << "Unable to connect: Timeout";

			ConnectFail(ERR_CLIENT_TIMEOUT);

			return false;
		}

		// If time to repost hello packet,
		if (now - last_hello_post >= hello_post_interval)
		{
			if (!PostHello())
			{
				WARN("Client") << "Unable to connect: Post failure";

				ConnectFail(ERR_CLIENT_BROKEN_PIPE);

				return false;
			}

			last_hello_post = now;
			hello_post_interval *= 2;
		}

		OnTick(&tls, now);
	}

	// Begin MTU probing after connection completes
	u32 mtu_discovery_time = Clock::msec();
	int mtu_discovery_attempts = 2;

	if (!DontFragment())
	{
		WARN("Client") << "Unable to detect MTU: Unable to set DF bit";

		mtu_discovery_attempts = 0;
	}
	else if (!PostMTUProbe(&tls, MAXIMUM_MTU) ||
			 !PostMTUProbe(&tls, MEDIUM_MTU))
	{
		WARN("Client") << "Unable to detect MTU: First probe post failure";
	}

	// Time synchronization begins right away
	u32 next_sync_time = Clock::msec();
	u32 sync_attempts = 0;

	// Set last receive time to avoid disconnecting due to timeout too soon
	_last_recv_tsc = next_sync_time;

	// While waiting for quit signal,
	while (!_kill_flag.Wait(Transport::TICK_RATE))
	{
		u32 now = Clock::msec();

		TickTransport(&tls, now);

		// If it is time for time synch,
		if ((s32)(now - next_sync_time) >= 0)
		{
			PostTimePing();

			// Increase synch interval after the first 8 data points
			if (sync_attempts >= TIME_SYNC_FAST_COUNT)
				next_sync_time = now + TIME_SYNC_INTERVAL;
			else
			{
				next_sync_time = now + TIME_SYNC_FAST;
				++sync_attempts;
			}
		}

		// If MTU discovery attempts continue,
		if (mtu_discovery_attempts > 0)
		{
			// If it is time to re-probe the MTU,
			if (now - mtu_discovery_time >= MTU_PROBE_INTERVAL)
			{
				// If payload bytes already maxed out,
				if (_max_payload_bytes >= MAXIMUM_MTU - _overhead_bytes)
				{
					// Stop posting probes
					mtu_discovery_attempts = 0;

					// On final attempt set DF=0
					DontFragment(false);
				}
				else
				{
					// If not on final attempt,
					if (mtu_discovery_attempts > 1)
					{
						// Post probes
						if (!PostMTUProbe(&tls, MAXIMUM_MTU - _overhead_bytes) ||
							!PostMTUProbe(&tls, MEDIUM_MTU - _overhead_bytes))
						{
							WARN("Client") << "Unable to detect MTU: Probe post failure";
						}

						mtu_discovery_time = now;
						--mtu_discovery_attempts;
					}
					else
					{
						// Stop posting probes
						mtu_discovery_attempts = 0;

						// On final attempt set DF=0
						DontFragment(false);
					}
				}
			}
		}

		// If no packets have been received,
		if ((s32)(now - _last_recv_tsc) >= TIMEOUT_DISCONNECT)
		{
			Disconnect(DISCO_TIMEOUT, true);

			return true;
		}

		// Tick subclass
		OnTick(&tls, now);

		// Send a keep-alive after the silence limit expires
		if ((s32)(now - _last_send_mstsc) >= SILENCE_LIMIT)
		{
			PostTimePing();

			next_sync_time = now + TIME_SYNC_INTERVAL;
		}
	}

	return true;
}

bool Client::PostPacket(u8 *buffer, u32 buf_bytes, u32 msg_bytes)
{
	if (!_auth_enc.Encrypt(buffer, buf_bytes, msg_bytes))
	{
		WARN("Client") << "Encryption failure while sending packet";

		AsyncBuffer::Release(buffer);

		return false;
	}

	if (Post(_server_addr, buffer, msg_bytes))
	{
		_last_send_mstsc = Clock::msec_fast();

		return true;
	}

	return false;
}

void Client::OnInternal(ThreadPoolLocalStorage *tls, BufferStream data, u32 bytes)
{
	switch (data[0])
	{
	case IOP_S2C_MTU_SET:
		if (bytes == IOP_S2C_MTU_SET_LEN)
		{
			u16 max_payload_bytes = getLE(*reinterpret_cast<u16*>( data + 1 ));

			WARN("Client") << "Got IOP_S2C_MTU_SET.  Max payload bytes = " << max_payload_bytes;

			// If new maximum payload is greater than the previous one,
			if (max_payload_bytes > _max_payload_bytes)
			{
				// Set max payload bytes
				_max_payload_bytes = max_payload_bytes;
			}
		}
		break;

	case IOP_S2C_TIME_PONG:
		if (bytes == IOP_S2C_TIME_PONG_LEN)
		{
			u32 client_now = Clock::msec();
			u32 *timestamps = reinterpret_cast<u32*>( data + 1 );
			u32 client_ping_send_time = timestamps[0];
			u32 server_ping_recv_time = getLE(timestamps[1]);
			u32 rtt = client_now - client_ping_send_time;

			u32 expected_test = ToServerTime(client_now, client_now - (rtt / 2));

			WARN("Client") << "Got IOP_S2C_TIME_PONG.  server_ping_recv_time=" << server_ping_recv_time << " Expected=" << expected_test;

			if (_ts_sample_count > 0)
			{
				ostringstream oss;
				oss << "timedata";
				oss << GetPort();
				oss << ".csv";
				ofstream file(oss.str(), ios::app);

				u32 delta = ToServerTime(client_now, 0);

				file << server_ping_recv_time << ", " << expected_test << ", " << rtt << ", " << delta << ", " << _ts_delta_test1 << ", " << _ts_delta_test2 << endl;
			}

			// If RTT is not impossible,
			if (rtt < TIMEOUT_DISCONNECT)
			{
				// delta = (Server ping receive time) - (Client ping send time) - (RTT / 2)
				s32 delta = server_ping_recv_time - client_ping_send_time - (rtt / 2);

				UpdateTimeSynch(client_now, rtt, delta);

				OnTimestampDeltaUpdate();
			}
		}
		break;

	case IOP_DISCO:
		if (bytes == IOP_DISCO_LEN)
		{
			WARN("Client") << "Got IOP_DISCO reason = " << (int)data[1];

			Disconnect(data[1], false);
		}
		break;
	}
}

void Client::Disconnect(u8 reason, bool notify)
{
	if (Atomic::Set(&_destroyed, 1) == 0)
	{
		if (notify)
			PostDisconnect(reason);

		TransportDisconnected();

		OnDisconnect(reason);

		_kill_flag.Set();

		Close();
	}
}

void Client::ConnectFail(HandshakeError err)
{
	if (Atomic::Set(&_destroyed, 1) == 0)
	{
		TransportDisconnected();

		OnConnectFail(err);

		_kill_flag.Set();

		Close();
	}
}

/*
	Clock Synchronization

	--Definition of Clock Delta:

	There is a roughly linear relationship between server and client time
	because both clocks are trying to tick once per millisecond.

	Clock Delta = (Server Remote Time) - (Client Local Time)

	--Measurements:

	Clients are responsible for synchronizing their clocks with the server,
	so that the server does not need to store this state for each user.

	At least every 20 seconds the client will ping the server to collect a
	timing sample (IOP_C2S_TIME_PING and IOP_S2C_TIME_PONG).

	After initial connection, the first 8 measurements are performed at 5
	second intervals to build up confidence in the time synchronization faster.

	T0 = Client ping send time (in client units)
	T1 = Server pong send time (in server units)
	T1' = Client time when server sent the pong (in client units)
	T2 = Client pong receive time (in client units)

	Round trip time (RTT) = T2 - T0
	Clock Delta = T1 - T1'

	--Calculation of Clock Delta:

	Assuming balanced ping and pong delays:

	T1' ~= T0 + RTT / 2
	Clock Delta = T1 - ((T2 - T0) / 2 + T0)

	--Incorporating 1st Order Drift:

	Delta drifts over time since the clocks do not tick at the same rate.
	Since the only clock available to measure the drift is on the client,
	delta is written in terms of the client local time.

	Di = Clock Delta i
	Ti = Client Local Time i (Time when server pong is received)
	Di = B0 * Ti + B1

	Assuming for now that all clock delta measurements have
	normally-distributed error about a true mean, and using the least
	square method to estimate error in B0, B1:

	Error E = Sum{ [ Di - (B0 * Ti + B1) ] ^ 2 }

	After optimizing with respect to B0 and B1, and a lot of algebra:

	T_avg = Average value of Ti
	D_avg = Average value of Di

	B0 = Sum[ (Ti - T_avg) * (Di - D_avg) ]
	     ----------------------------------
		       Sum[ (Ti - T_avg) ^ 2 ]

	B1 = D_avg - B0 * T_avg

	If only one measurement is available, then B0 = 0 and it simplifies
	to the single measurement case without drift taken into account.

	At least two samples are required to calculate relative clock drift.

	The client may now convert from local time to server time and back:

	Server Time = (Client Time) + (B0 * (Client Request Time) + B1)
	Client Time = (Server Time) - (B0 * (Client Request Time) + B1)

	Accounting for drift will defeat naive speed cheats since the increased
	clock tick rate will be corrected out just like normal drift.

	--Incorporating Measurement Quality:

	Not all deltas are of equal quality.  When the ping and/or pong delays
	are higher for various practical reasons, the delays become unbalanced
	and this estimate of clock delta loses accuracy.  Fortunately the RTT
	will also increase when this occurs, and so measurements with lower RTT
	may be considered more accurate.

	Throwing away 75% of the measurements, keeping the lowest RTT samples,
	should prevent bad data from affecting the clock synchronization.

	--Incorporating Timestamp Rollover:

	The timestamps are 32 bits and will roll over about once every 4 days.
	To handle the rollover case, when including any timestamp in a formula,
	just make it relative to a recent timestamp.  Any timestamp older than that
	will cause rollover.

	--Incorporating Thread Safety:

	Since converting a timestamp now requires multiple variables, the updating
	of these variables needs to be synchronous with their usage.  So protect
	them with a mutex.

	--Incorporating Drift Uncertainty:

	It usually takes many seconds to see any appreciable clock drift.  But due
	to the uncertainty in the measurements, deltas may be off by a few
	milliseconds.  If the time period of the measurements is just a few seconds,
	then a millisecond may appear to be a large clock drift that will actually
	be detrimental to synchronization.

	As a result, the drift calculations should only be employed after a
	reasonable amount of time has passed.  This is enforced in my code by
	requiring at least 4 samples before enabling the drift calculations.
*/
void Client::UpdateTimeSynch(u32 pong_time, u32 rtt, s32 delta)
{
	// Increment the sample count if we haven't exhausted the array space yet
	if (_ts_sample_count < MAX_TS_SAMPLES)
		_ts_sample_count++;

	// Insert sample
	_ts_samples[_ts_next_index].delta = delta;
	_ts_samples[_ts_next_index].when = pong_time;
	_ts_samples[_ts_next_index].rtt = rtt;

	// Increment write address to next oldest entry or next empty space
	_ts_next_index = (_ts_next_index + 1) % MAX_TS_SAMPLES;

	// If only one measurement we cannot calculate drift yet
	if (_ts_sample_count <= 1)
	{
		_ts_lock.Enter();
		_ts_B0 = 0.;
		_ts_B1 = delta;
		_ts_lock.Leave();

		WARN("Client") << "SKIPPED DRIFT (ONLY ONE SAMPLE) B0 = " << _ts_B0 << " B1 = " << _ts_B1;
		return; // Skip drift calculation
	}

	// Find the lowest 25% RTT samples >= MIN_TS_SAMPLES
	TimesPingSample *BestSamples[MAX_TS_SAMPLES / 4 + MIN_TS_SAMPLES];
	u32 num_samples = _ts_sample_count / 4;
	if (num_samples < MIN_TS_SAMPLES) num_samples = MIN_TS_SAMPLES;

	// Find the highest RTT sample so far
	u32 highest_rtt = _ts_samples[0].rtt, highest_index = 0;
	BestSamples[0] = &_ts_samples[0];
	u32 best_count = 1;

	// While still trying to fill a minimum number of samples,
	u32 ii;
	for (ii = 1; ii < _ts_sample_count && best_count < num_samples; ++ii)
	{
		u32 rtt = _ts_samples[ii].rtt;

		if (rtt > highest_rtt)
		{
			highest_rtt = rtt;
			highest_index = ii;
		}

		BestSamples[best_count++] = &_ts_samples[ii];
	}

	// For the remainder of the samples, fight over spots
	for (; ii < _ts_sample_count; ++ii)
	{
		u32 sample_rtt = _ts_samples[ii].rtt;

		// Replace highest RTT if the new sample has lower RTT
		if (sample_rtt < highest_rtt)
		{
			BestSamples[highest_index] = &_ts_samples[ii];

			// Find new highest RTT entry
			highest_rtt = sample_rtt;

			for (u32 jj = 0; jj < best_count; ++jj)
			{
				u32 rtt = BestSamples[jj]->rtt;

				if (rtt > highest_rtt)
				{
					highest_rtt = rtt;
					highest_index = jj;
				}
			}
		}
	}

	// If there are not enough measurements to estimate drift,
	//if (best_count < MIN_DRIFT_SAMPLES) (TEST1)
	{
		WARN("Client") << "Calculating average delta:";

		// Calculate the average delta and assume no drift yet
		s64 sum_delta = 0;

		for (ii = 0; ii < best_count; ++ii)
		{
			WARN("Client") << ii << " - when=" << BestSamples[ii]->when << " rtt=" << BestSamples[ii]->rtt << " delta=" << BestSamples[ii]->delta;

			sum_delta += BestSamples[ii]->delta;

			// If the new delta is one of the best then use it (TEST2)
			if (BestSamples[ii]->delta == delta)
			{
				_ts_delta_test2 = delta;
			}
		}

		u32 avg_delta = (u32)(sum_delta / best_count);

		_ts_lock.Enter();
		_ts_B0 = 0.;
		_ts_B1 = avg_delta;
		_ts_lock.Leave();

		WARN("Client") << "SKIPPED DRIFT (NOT ENOUGH MEASUREMENTS, USING AVERAGE) B0 = " << _ts_B0 << " B1 = " << _ts_B1;

		_ts_delta_test1 = avg_delta;

		//return; // Skip drift calculations
	}

	if (best_count < MIN_DRIFT_SAMPLES)
		return;

	// Choose a time in the past as a base point to fix rollover problem
	u32 base_time = pong_time - MAX_TS_SAMPLES * TIME_SYNC_INTERVAL - TIME_SYNC_INTERVAL;

	// Calculate average time and delta
	s64 sum_when = BestSamples[0]->when - base_time, sum_delta = BestSamples[0]->delta;

	for (ii = 1; ii < best_count; ++ii)
	{
		sum_when += (s32)(BestSamples[ii]->when - base_time);
		sum_delta += BestSamples[ii]->delta;
	}

	// Calculate B0 numerator and denominator
	s64 B0_numerator = 0, B0_denominator = 0;

	WARN("Client") << "Calculating drift:";

	for (ii = 0; ii < best_count; ++ii)
	{
		WARN("Client") << ii << " - when=" << BestSamples[ii]->when << " rtt=" << BestSamples[ii]->rtt << " delta=" << BestSamples[ii]->delta;

		s64 when_term = (u32)(BestSamples[ii]->when - base_time) * (s64)best_count - sum_when;
		s64 delta_term = BestSamples[ii]->delta * (s64)best_count - sum_delta;

		B0_numerator += when_term * delta_term;
		B0_denominator += when_term * when_term;
	}

	// If the denominator indicates no drift,
	if (B0_denominator <= 0)
	{
		_ts_lock.Enter();
		_ts_B0 = 0.;
		_ts_B1 = delta;
		_ts_lock.Leave();

		WARN("Client") << "SKIPPED DRIFT (DENOMINATOR = 0) B0 = " << _ts_B0 << " B1 = " << _ts_B1;

		return; // Skip drift calculation
	}

	// Calculate B0 and B1
	_ts_lock.Enter();
	_ts_base = base_time;
	_ts_B0 = B0_numerator / (double)B0_denominator;
	_ts_B1 = (s32)((sum_delta - _ts_B0 * sum_when) / (s64)best_count);
	_ts_lock.Leave();

	WARN("Client") << "B0 = " << _ts_B0 << " B1 = " << _ts_B1;
}
