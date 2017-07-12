//---------------------------------------------------------------------------
// Copyright (c) 2017 Michael G. Brehm
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//---------------------------------------------------------------------------

#include "stdafx.h"
#include "livestream.h"

#include <algorithm>
#include <assert.h>
#include <chrono>
#include <string.h>
#include <type_traits>

#include "string_exception.h"

#pragma warning(push, 4)

// align_up (local)
//
// Aligns a size_t up to a specified boundary
static size_t align_up(size_t value, unsigned int alignment)
{
	if(alignment < 1) throw std::out_of_range("alignment");
	return (value == 0) ? 0 : value + ((alignment - (value % alignment)) % alignment);
}

//---------------------------------------------------------------------------
// livestream Constructor
//
// Arguments:
//
//	buffersize	- Size in bytes of the stream ring buffer

livestream::livestream(size_t buffersize) : m_buffersize(align_up(buffersize + WRITE_PADDING, 65536))
{
	// Allocate the live stream ring buffer
	m_buffer.reset(new uint8_t[m_buffersize]);
	if(!m_buffer) throw std::bad_alloc();

	// Allocate the buffer for any reported CURL error messages
	m_curlerr.reset(new char[CURL_ERROR_SIZE]);
	if(!m_curlerr) throw std::bad_alloc();
}

//---------------------------------------------------------------------------
// livestream Destructor

livestream::~livestream()
{
	stop();							// Stop the live stream
}

//---------------------------------------------------------------------------
// livestream::curl_responseheaders (static, private)
//
// libcurl callback to process response headers
//
// Arguments:
//
//	data		- Pointer to the response header data
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t livestream::curl_responseheaders(char const* data, size_t size, size_t count, void* context)
{
	static const char CONTENT_RANGE_HEADER[]		= "Content-Range:";
	static const size_t CONTENT_RANGE_HEADER_LEN	= strlen(CONTENT_RANGE_HEADER);

	size_t cb = size * count;				// Calculate the actual byte count
	if(cb == 0) return 0;					// Nothing to do

	// Cast the context pointer back into a livestream instance
	livestream* instance = reinterpret_cast<livestream*>(context);

	// Content-Range:
	//
	if((cb >= CONTENT_RANGE_HEADER_LEN) && (strncmp(CONTENT_RANGE_HEADER, data, CONTENT_RANGE_HEADER_LEN) == 0)) {

		unsigned long long rangestart = 0;

		// Copy the header data into a local buffer to ensure null termination, which is not guaranteed
		std::unique_ptr<char[]> buffer(new char[cb + 1]);
		memcpy(&buffer[0], data, cb);
		buffer[cb] = 0;

		// The Content-Range header gives us the starting position of the stream from the server's
		// perspective which is used to normalize the reported stream position
		if(sscanf(data, "Content-Range: bytes %llu-", &rangestart) == 1) {
			
			// When the stream (re)starts, set the initial stream positions
			instance->m_startpos = instance->m_readpos = instance->m_writepos = rangestart;
		}
	}

	return cb;
}

//---------------------------------------------------------------------------
// livestream::curl_transfercontrol (static, private)
//
// libcurl callback to handle transfer information/progress
//
// Arguments:
//
//	context		- Caller-provided context pointer
//	dltotal		- Number of bytes expected to be downloaded
//	dlnow		- Number of bytes already downloaded
//	ultotal		- Number of bytes expected to be uploaded
//	ulnow		- Number of bytes already uploaded

int livestream::curl_transfercontrol(void* context, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/)
{
	// Cast the livestream instance pointer out from the context pointer
	livestream* instance = reinterpret_cast<livestream*>(context);

	// If a stop has been signaled, terminate the transfer
	if(instance->m_stop.exchange(false) == true) return -1;

	// Automatically resume a paused data transfer when this notification callback is invoked
	if(instance->m_paused.exchange(false) == true) curl_easy_pause(instance->m_curl, CURLPAUSE_CONT);

	return 0;
}

//---------------------------------------------------------------------------
// livestream::curl_write (static, private)
//
// libcurl callback to write transferred data into the buffer
//
// Arguments:
//
//	data		- Pointer to the data to be written
//	size		- Size of a single data element
//	count		- Number of data elements
//	context		- Caller-provided context pointer

size_t livestream::curl_write(void const* data, size_t size, size_t count, void* context)
{
	size_t				cb = size * count;			// Calculate the actual byte count
	size_t				byteswritten = 0;			// Total bytes actually written

	if((data == nullptr) || (cb == 0) || (context == nullptr)) return 0;

	// Cast the context pointer back into a livestream instance
	livestream* instance = reinterpret_cast<livestream*>(context);

	// To support seeking within the ring buffer, some level of synchronization
	// is required here -- there should be almost no contention on this lock
	std::unique_lock<std::mutex> writelock(instance->m_writelock);

	// Copy the current head and tail positions, this works without a lock by operating
	// on the state of the buffer at the time of the request
	size_t head = instance->m_bufferhead;
	size_t tail = instance->m_buffertail;

	// This operation requires that all of the data be written, if it isn't going to fit in the
	// available ring buffer space, the input stream has to be paused via CURL_WRITEFUNC_PAUSE
	size_t available = (head < tail) ? tail - head : (instance->m_buffersize - head) + tail;
	if(available < (cb + WRITE_PADDING)) { instance->m_paused.store(true); return CURL_WRITEFUNC_PAUSE; }

	// Write until the buffer has been exhausted or the desired count has been reached
	while(cb) {

		// If the head is behind the tail linearly, take the data between them otherwise
		// take the data between the end of the buffer and the head
		size_t chunk = (head < tail) ? std::min(cb, tail - head) : std::min(cb, instance->m_buffersize - head);
		memcpy(&instance->m_buffer[head], &reinterpret_cast<uint8_t const*>(data)[byteswritten], chunk);

		head += chunk;					// Increment the head position
		byteswritten += chunk;			// Increment number of bytes written
		cb -= chunk;					// Decrement remaining bytes

		// If the head has reached the end of the buffer, reset it back to zero
		if(head >= instance->m_buffersize) head = 0;
	}

	// Modify the atomic<> head position after the operation has completed and notify
	// that a write operation against the ring buffer has completed (head has changed)
	instance->m_bufferhead.store(head);
	instance->m_bufferhasdata.notify_all();

	// All of the data should have been written into the ring buffer
	assert(byteswritten == (size * count));

	// Increment the number of bytes seen as part of this transfer, and if
	// we have exceeded the previously known length update that as well
	instance->m_writepos += byteswritten;
	if(instance->m_writepos > instance->m_length.load()) instance->m_length.store(instance->m_writepos);

	// Release the thread waiting for the transfer to start after some data is
	// available to be read from the buffer to avoid initial reader starvation
	instance->m_started = true;

	return byteswritten;
}

//---------------------------------------------------------------------------
// livestream::length
//
// Gets the length of the live stream as transferred thus far
//
// Arguments:
//
//	NONE

uint64_t livestream::length(void) const
{
	return m_length.load();
}
	
//---------------------------------------------------------------------------
// livestream::position
//
// Gets the current position within the live stream
//
// Arguments:
//
//	NONE

uint64_t livestream::position(void) const
{
	std::unique_lock<std::mutex> lock(m_lock);
	return m_readpos;
}

//---------------------------------------------------------------------------
// livestream::read
//
// Reads data from the live stream
//
// Arguments:
//
//	buffer		- Buffer to receive the live stream data
//	count		- Size of the destination buffer in bytes
//	timeoutms	- Amount of time to wait for the read to succeed

size_t livestream::read(uint8_t* buffer, size_t count, uint32_t timeoutms)
{
	size_t				bytesread = 0;			// Total bytes actually read
	size_t				head = 0;				// Current head position
	size_t				tail = 0;				// Current tail position

	if(buffer == nullptr) throw std::invalid_argument("buffer");
	if(count > m_buffersize) throw std::invalid_argument("count");
	if(count == 0) return 0;

	std::unique_lock<std::mutex> lock(m_lock);

	// Wait up to the specified amount of time for any data to be available in the ring buffer
	if(!m_bufferhasdata.wait_until(lock, std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutms), [&]() -> bool { 
	
		head = m_bufferhead;				// Copy the atomic<> head position
		tail = m_buffertail;				// Copy the atomic<> tail position

		// Padding was added in curl_write to ensure that the buffer can never become
		// full, therefore if the head is the same as the tail the buffer is empty
		return (head != tail);

	})) return 0;

	// Read until the buffer has been exhausted or the desired count has been reached
	while(count) {

		// If the tail is behind the head linearly, take the data between them otherwise
		// take the data between the end of the buffer and the tail
		size_t chunk = (tail < head) ? std::min(count, head - tail) : std::min(count, m_buffersize - tail);
		memcpy(&buffer[bytesread], &m_buffer[tail], chunk);

		tail += chunk;					// Increment the tail position
		bytesread += chunk;				// Increment number of bytes read
		count -= chunk;					// Decrement remaining bytes

		// If the tail has reached the end of the buffer, reset it back to zero
		if(tail >= m_buffersize) tail = 0;

		// If the tail has reached the head, the buffer is now empty
		if(tail == head) break;
	}

	// Modify the atomic<> tail position after the operation has completed
	// and increment the current position of the stream beyond what was read
	m_buffertail.store(tail);
	m_readpos += bytesread;

	return bytesread;
}

//---------------------------------------------------------------------------
// livestream::reset_stream_state (private)
//
// Resets the state of the live stream transfer
//
// Arguments:
//
//	lock		- Reference to unique_lock<> that must be owned

void livestream::reset_stream_state(std::unique_lock<std::mutex> const& lock)
{
	// The lock argument is necessary to ensure the caller owns it before proceeding
	if(!lock.owns_lock()) throw string_exception(__func__, ": caller does not own the unique_lock<>");

	// The worker thread must not be running when the stream state is reset
	if(m_worker.joinable()) throw string_exception(__func__, ": cannot reset an active data transfer");

	// Reset the stream control flags
	m_started = false;
	m_paused.store(false);
	m_stop.store(false);

	// Reset the stream positions, but leave length intact -- length represents the
	// largest position seen while streaming the content
	m_startpos = m_readpos = m_writepos = 0;

	// Reset the ring buffer back to an empty state
	m_bufferhead.store(0);
	m_buffertail.store(0);
}

//---------------------------------------------------------------------------
// livestream::seek
//
// Stops and restarts the data transfer at a specific position
//
// Arguments:
//
//	position		- Requested position for the seek operation

uint64_t livestream::seek(uint64_t position)
{
	std::unique_lock<std::mutex> lock(m_lock);

	// If the position is the same as the current position, there is nothing to do
	if(position == m_readpos) return position;

	// The transfer must be active prior to the seek operation
	if(!m_worker.joinable()) throw string_exception(__func__, ": cannot seek an inactive data transfer");

	// Take the write lock to prevent any changes to the head position while calculating
	// if the seek can be fulfilled with the data already in the buffer
	std::unique_lock<std::mutex> writelock(m_writelock);

	// Calculate the minimum position represented in the ring buffer
	uint64_t minpos = ((m_writepos - m_startpos) > m_buffersize) ? m_writepos - m_buffersize : m_startpos;

	if((position >= minpos) && (position <= m_writepos)) {

		// If the buffer hasn't wrapped around yet, the new tail position is relative to buffer[0]
		if(minpos == m_startpos) m_buffertail = static_cast<size_t>(position - m_startpos);

		else {

			size_t tail = m_bufferhead;					// Start at the head (minpos)
			uint64_t delta = position - minpos;			// Calculate the required delta

			// Set the new tail position; if the delta is larger than the remaining space in the
			// buffer it is relative to buffer[0], otherwise it is relative to buffer[minpos]
			m_buffertail = static_cast<size_t>((delta >= (m_buffersize - tail)) ? delta - (m_buffersize - tail) : tail + delta);
		}

		// The stream was seeked within the buffer, set the new read (tail) position
		m_readpos = position;
		return position;
	}

	writelock.unlock();						// Release the write lock
	m_stop.store(true);						// Signal worker thread to stop
	m_worker.join();						// Wait for it to actually stop

	reset_stream_state(lock);				// Reset the stream state

	// Format the updated RANGE header value and apply it to the original transfer object
	char byterange[32];
	snprintf(byterange, std::extent<decltype(byterange)>::value, "%llu-", static_cast<unsigned long long>(position));

	if(curl_easy_setopt(m_curl, CURLOPT_RANGE, byterange) != CURLE_OK) {
	
		// If CURLOPT_RANGE couldn't be applied to the existing transfer object stop the transfer
		// by closing out the existing CURL object as would be done by the stop() method

		curl_easy_cleanup(m_curl);
		m_curl = nullptr;

		throw string_exception(__func__, ": curl_easy_setopt() failed; transfer stopped");
	}

	// Create a new worker thread on which to perform the transfer operations and wait for it to start
	m_worker = std::move(std::thread(&livestream::transfer_func, this));
	m_started.wait_until_equals(true);

	// If the transfer thread failed to initiate the data transfer throw an exception
	if(m_curlresult != CURLE_OK) 
		throw string_exception(__func__, ": failed to restart transfer at position ", position, ": ", &m_curlerr[0]);

	// Return the starting position of the stream
	return m_readpos;
}

//---------------------------------------------------------------------------
// livestream::start
//
// Arguments:
//
//	url			- URL of the live stream to be opened

uint64_t livestream::start(char const* url)
{
	if(url == nullptr) throw std::invalid_argument("url");

	std::unique_lock<std::mutex> lock(m_lock);

	// Check to make sure that the worker thread isn't already running
	if(m_worker.joinable()) throw string_exception(__func__, ": data transfer is already active");

	// Create and initialize the libcurl easy interface for the specified URL
	m_curl = curl_easy_init();
	if(m_curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

	try {

		// Set the options for the easy interface curl object
		CURLcode curlresult = curl_easy_setopt(m_curl, CURLOPT_URL, url);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_FAILONERROR, 1L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, &livestream::curl_responseheaders);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &livestream::curl_write);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_XFERINFOFUNCTION, &livestream::curl_transfercontrol);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_XFERINFODATA, this);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_NOPROGRESS, 0L);
		if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(m_curl, CURLOPT_ERRORBUFFER, m_curlerr);
		if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed");

		// Create a new worker thread on which to perform the transfer operations and wait for it to start
		m_worker = std::move(std::thread(&livestream::transfer_func, this));
		m_started.wait_until_equals(true);

		// If the transfer thread failed to initiate the data transfer throw an exception
		if(m_curlresult != CURLE_OK) 
			throw string_exception(__func__, ": failed to start transfer for url ", url, ": ", &m_curlerr[0]);

		// Return the starting position of the stream
		return m_readpos;
	}
		
	catch(...) { curl_easy_cleanup(m_curl); m_curl = nullptr; throw; }
}

//---------------------------------------------------------------------------
// livestream::stop
//
// Stops the data transfer into the live stream buffer
//
// Arguments:
//
//	NONE

uint64_t livestream::stop(void)
{
	uint64_t			position;			// Position at which stream stopped

	std::unique_lock<std::mutex> lock(m_lock);

	// If the worker thread is not running, the transfer has already stopped; don't
	// throw an exception just return a nice peaceful zero to the caller
	if(!m_worker.joinable()) return 0;

	// Signal the worker thread to stop and wait for it to do so
	m_stop.store(true);
	m_worker.join();

	// Grab the final position of the stream before resetting
	position = m_readpos;

	// Reset the stream state as well as the length
	reset_stream_state(lock);
	m_length.store(0);

	// Clean up the CURL easy interface object
	curl_easy_cleanup(m_curl);
	m_curl = nullptr;

	// Return the final position of the stream
	return position;
}

//---------------------------------------------------------------------------
// livestream::transfer_func (private)
//
// Worker thread procedure for the CURL data transfer instance
//
// Arguments:
//
//	NONE

void livestream::transfer_func(void)
{
	// Initialize the result code and clear out the CURL error buffer
	m_curlresult = CURLE_OK;
	memset(&m_curlerr[0], 0, CURL_ERROR_SIZE);

	// Attempt to execute the current transfer operation, wait for the
	// operation to complete or to fail prematurely with an error code
	m_curlresult = curl_easy_perform(m_curl);
	m_started = true;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
