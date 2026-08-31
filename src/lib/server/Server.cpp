/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "server/Server.h"

#include "server/ClientProxy.h"
#include "server/ClientProxyUnknown.h"
#include "server/PrimaryClient.h"
#include "server/ClientListener.h"
#include "barrier/FileChunk.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/DropHelper.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/XScreen.h"
#include "barrier/XBarrier.h"
#include "barrier/StreamChunker.h"
#include "barrier/KeyState.h"
#include "barrier/Screen.h"
#include "barrier/PacketStreamFilter.h"
#include "barrier/DisplayGeometry.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/XSocket.h"
#include "mt/Thread.h"
#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <ctime>
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace barrier {

namespace {

// GUI link serialization rounds every interval endpoint to a whole
// percentage (qRound(f * 100)); a valid interval may therefore overhang
// this display's exact edge span by up to one encoded step.
const float kIntervalQuantizationTolerance = 0.01f;

// Union-relative fraction of a point along the axis perpendicular to
// \p direction, mirroring Server::mapToFraction() against the union
// bounding box.  Configured link intervals are encoded against this
// same union frame, so the cursor fraction must be computed here.
float edgeFraction(EDirection direction, const ScreenRect& unionRect,
                   SInt32 cursorX, SInt32 cursorY)
{
	switch (direction) {
	case kLeft:
	case kRight:
		if (unionRect.h <= 0) {
			return 0.0f;
		}
		return static_cast<float>(cursorY - unionRect.y + 0.5f) /
		       static_cast<float>(unionRect.h);
	case kTop:
	case kBottom:
		if (unionRect.w <= 0) {
			return 0.0f;
		}
		return static_cast<float>(cursorX - unionRect.x + 0.5f) /
		       static_cast<float>(unionRect.w);
	default:
		return 0.0f;
	}
}

// Fraction span of \p display's own edge along the axis perpendicular
// to \p direction.  Only a link whose interval lies within this span
// can belong to this display's edge; intervals of other displays in the
// union must never be reached from here.
void displayEdgeSpan(const ScreenRect& display, EDirection direction,
                     const ScreenRect& unionRect, float& start, float& end)
{
	switch (direction) {
	case kLeft:
	case kRight:
		if (unionRect.h <= 0) {
			start = 0.0f;
			end = 0.0f;
			return;
		}
		start = static_cast<float>(display.y - unionRect.y) /
		        static_cast<float>(unionRect.h);
		end = static_cast<float>(display.y + display.h - unionRect.y) /
		      static_cast<float>(unionRect.h);
		break;
	case kTop:
	case kBottom:
		if (unionRect.w <= 0) {
			start = 0.0f;
			end = 0.0f;
			return;
		}
		start = static_cast<float>(display.x - unionRect.x) /
		        static_cast<float>(unionRect.w);
		end = static_cast<float>(display.x + display.w - unionRect.x) /
		      static_cast<float>(unionRect.w);
		break;
	default:
		start = 0.0f;
		end = 0.0f;
		break;
	}
}

// True when another display of the same screen abuts \p display's
// \p direction edge at the cursor's coordinate along that edge.
bool sameHostAbuts(const ScreenRect& display, EDirection direction,
                   const std::vector<ScreenRect>& displays,
                   SInt32 cursorX, SInt32 cursorY)
{
	for (std::vector<ScreenRect>::const_iterator it = displays.begin();
			it != displays.end(); ++it) {
		const ScreenRect& other = *it;
		if (other.x == display.x && other.y == display.y &&
				other.w == display.w && other.h == display.h) {
			// the display itself
			continue;
		}
		SInt32 edge, otherEdge, spanStart, spanEnd;
		switch (direction) {
		case kLeft:
			edge = display.x;
			otherEdge = other.x + other.w;
			spanStart = std::max(display.y, other.y);
			spanEnd = std::min(display.y + display.h, other.y + other.h);
			break;
		case kRight:
			edge = display.x + display.w;
			otherEdge = other.x;
			spanStart = std::max(display.y, other.y);
			spanEnd = std::min(display.y + display.h, other.y + other.h);
			break;
		case kTop:
			edge = display.y;
			otherEdge = other.y + other.h;
			spanStart = std::max(display.x, other.x);
			spanEnd = std::min(display.x + display.w, other.x + other.w);
			break;
		case kBottom:
			edge = display.y + display.h;
			otherEdge = other.y;
			spanStart = std::max(display.x, other.x);
			spanEnd = std::min(display.x + display.w, other.x + other.w);
			break;
		default:
			continue;
		}
		if (edge != otherEdge) {
			continue;
		}
		const SInt32 cursorAlong = (direction == kLeft || direction == kRight)
		                               ? cursorY : cursorX;
		if (spanEnd > spanStart && cursorAlong >= spanStart &&
				cursorAlong < spanEnd) {
			return true;
		}
	}
	return false;
}

} // namespace

BoundaryOwner
classifyDisplayBoundary(const ScreenRect& sourceDisplay, EDirection direction,
                        const std::vector<ScreenRect>& activeDisplays,
                        const std::vector<Config::Interval>& edgeLinkIntervals,
                        SInt32 cursorX, SInt32 cursorY)
{
	if (direction == kNoDirection || activeDisplays.empty()) {
		return BoundaryOwner::Clamp;
	}

	// A boundary shared with another display of the same screen is owned
	// by the local OS: the cursor moves natively, so Barrier must neither
	// switch nor clamp.
	if (sameHostAbuts(sourceDisplay, direction, activeDisplays,
			cursorX, cursorY)) {
		return BoundaryOwner::LocalOS;
	}

	// External edge.  Only a configured cross-host link covering this
	// physical edge coordinate makes the edge Remote.  Links are encoded
	// against the display union, so the cursor fraction is computed in
	// that same union frame, and a link only counts when its interval
	// lies within this display's own edge span -- an interval belonging
	// to another display of the union must not be reached from here.
	const ScreenRect unionRect = unionBounds(activeDisplays);
	const float fraction = edgeFraction(direction, unionRect,
	                                    cursorX, cursorY);
	float spanStart, spanEnd;
	displayEdgeSpan(sourceDisplay, direction, unionRect, spanStart, spanEnd);
	for (std::vector<Config::Interval>::const_iterator it =
			edgeLinkIntervals.begin(); it != edgeLinkIntervals.end(); ++it) {
		// The interval must belong to this display's edge: its endpoints
		// lie within the display's span, tolerating one encoded percent
		// step of GUI rounding so a valid link is not rejected (e.g. an
		// exact 1440/2160 = 0.6667 endpoint serializes as 0.67).
		if (it->first >= spanStart - kIntervalQuantizationTolerance &&
				it->second <= spanEnd + kIntervalQuantizationTolerance &&
				fraction >= it->first && fraction < it->second) {
			return BoundaryOwner::Remote;
		}
	}
	return BoundaryOwner::Clamp;
}

} // namespace barrier

//
// Server
//

Server::Server(
		Config& config,
		PrimaryClient* primaryClient,
		barrier::Screen* screen,
		IEventQueue* events,
		ServerArgs const& args) :
	m_mock(false),
	m_primaryClient(primaryClient),
	m_active(primaryClient),
	m_seqNum(0),
	m_xDelta(0),
	m_yDelta(0),
	m_xDelta2(0),
	m_yDelta2(0),
	m_config(&config),
	m_displayTopologyTimer(NULL),
	m_switchingEnabled(false),
	m_inputFilter(config.getInputFilter()),
	m_activeSaver(NULL),
	m_switchDir(kNoDirection),
	m_switchScreen(NULL),
	m_switchWaitDelay(0.0),
	m_switchWaitTimer(NULL),
	m_switchTwoTapDelay(0.0),
	m_switchTwoTapEngaged(false),
	m_switchTwoTapArmed(false),
	m_switchTwoTapZone(3),
	m_switchNeedsShift(false),
	m_switchNeedsControl(false),
	m_switchNeedsAlt(false),
	m_relativeMoves(false),
	m_keyboardBroadcasting(false),
	m_lockedToScreen(false),
	m_screen(screen),
	m_events(events),
	m_sendFileThread(NULL),
	m_writeToDropDirThread(NULL),
	m_ignoreFileTransfer(false),
	m_enableClipboard(true),
	m_sendDragInfoThread(NULL),
	m_waitDragInfoThread(true),
	m_args(args)
{
	// must have a primary client and it must have a canonical name
	assert(m_primaryClient != NULL);
	assert(config.isScreen(primaryClient->getName()));
	assert(m_screen != NULL);

    std::string primaryName = getName(primaryClient);

	// clear clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		ClipboardInfo& clipboard   = m_clipboards[id];
		clipboard.m_clipboardOwner  = primaryName;
		clipboard.m_clipboardSeqNum = m_seqNum;
		if (clipboard.m_clipboard.open(0)) {
			clipboard.m_clipboard.empty();
			clipboard.m_clipboard.close();
		}
		clipboard.m_clipboardData   = clipboard.m_clipboard.marshall();
	}

	// install event handlers
	m_events->adoptHandler(Event::kTimer, this,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchWaitTimeout));
	m_events->adoptHandler(Event::kTimer, &m_displayTopologyStateMachine,
							new TMethodEventJob<Server>(this,
								&Server::handleDisplayTopologyTimer));
	m_events->adoptHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyDownEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyUpEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyRepeatEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonDownEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonUpEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionPrimaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionSecondaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleWheelEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverActivatedEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverDeactivatedEvent));
	m_events->adoptHandler(m_events->forServer().switchToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchToScreenEvent));
  m_events->adoptHandler(m_events->forServer().toggleScreen(),
              m_inputFilter,
              new TMethodEventJob<Server>(this,
                &Server::handleToggleScreenEvent));
	m_events->adoptHandler(m_events->forServer().switchInDirection(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchInDirectionEvent));
	m_events->adoptHandler(m_events->forServer().keyboardBroadcast(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyboardBroadcastEvent));
	m_events->adoptHandler(m_events->forServer().lockCursorToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleLockCursorToScreenEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputBeginEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputEndEvent));

	if (m_args.m_enableDragDrop) {
		m_events->adoptHandler(m_events->forFile().fileChunkSending(),
								this,
								new TMethodEventJob<Server>(this,
									&Server::handleFileChunkSendingEvent));
		m_events->adoptHandler(m_events->forFile().fileRecieveCompleted(),
								this,
								new TMethodEventJob<Server>(this,
									&Server::handleFileRecieveCompletedEvent));
	}

	// add connection
	addClient(m_primaryClient);

	// set initial configuration
	setConfig(config);

	// enable primary client
	m_primaryClient->enable();
	m_inputFilter->setPrimaryClient(m_primaryClient);

	// Determine if scroll lock is already set. If so, lock the cursor to the primary screen
	if (m_primaryClient->getToggleMask() & KeyModifierScrollLock) {
		LOG((CLOG_NOTE "Scroll Lock is on, locking cursor to screen"));
		m_lockedToScreen = true;
	}

}

Server::~Server()
{
	if (m_mock) {
		return;
	}

	// remove event handlers and timers
	m_events->removeHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter);
	m_events->removeHandler(Event::kTimer, this);
	stopDisplayTopologyTimer();
	m_events->removeHandler(Event::kTimer, &m_displayTopologyStateMachine);
	stopSwitch();

	// force immediate disconnection of secondary clients
	disconnect();
	for (OldClients::iterator index = m_oldClients.begin();
							index != m_oldClients.end(); ++index) {
		BaseClientProxy* client = index->first;
		m_events->deleteTimer(index->second);
		m_events->removeHandler(Event::kTimer, client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		delete client;
	}

	// remove input filter
	m_inputFilter->setPrimaryClient(NULL);

	// disable and disconnect primary client
	m_primaryClient->disable();
	removeClient(m_primaryClient);
}

bool
Server::setConfig(const Config& config)
{
	// refuse configuration if it doesn't include the primary screen
	if (!config.isScreen(m_primaryClient->getName())) {
		return false;
	}

	// close clients that are connected but being dropped from the
	// configuration.
	closeClients(config);

	// cut over
	processOptions();

	// add ScrollLock as a hotkey to lock to the screen.  this was a
	// built-in feature in earlier releases and is now supported via
	// the user configurable hotkey mechanism.  if the user has already
	// registered ScrollLock for something else then that will win but
	// we will unfortunately generate a warning.  if the user has
	// configured a LockCursorToScreenAction then we don't add
	// ScrollLock as a hotkey.
	if (!m_config->hasLockToScreenAction()) {
		IPlatformScreen::KeyInfo* key =
			IPlatformScreen::KeyInfo::alloc(kKeyScrollLock, 0, 0, 0);
		InputFilter::Rule rule(new InputFilter::KeystrokeCondition(m_events, key));
		rule.adoptAction(new InputFilter::LockCursorToScreenAction(m_events), true);
		m_inputFilter->addFilterRule(rule);
	}

	// tell primary screen about reconfiguration
	m_primaryClient->reconfigure(getActivePrimarySides());

	// tell all (connected) clients about current options
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		sendOptions(client);
	}

	updateDisplayTopology(m_primaryClient->getDisplayTopology(),
						 displayTopologyMonotonicMs());

	return true;
}

void
Server::adoptClient(BaseClientProxy* client)
{
	assert(client != NULL);

	// watch for client disconnection
	m_events->adoptHandler(m_events->forClientProxy().disconnected(), client,
							new TMethodEventJob<Server>(this,
								&Server::handleClientDisconnected, client));

	// name must be in our configuration
	if (!m_config->isScreen(client->getName())) {
		LOG((CLOG_WARN "unrecognised client name \"%s\", check server config", client->getName().c_str()));
		closeClient(client, kMsgEUnknown);
		return;
	}

	// add client to client list
	if (!addClient(client)) {
		// can only have one screen with a given name at any given time
		LOG((CLOG_WARN "a client with name \"%s\" is already connected", getName(client).c_str()));
		closeClient(client, kMsgEBusy);
		return;
	}
	const std::string clientName = getName(client);
	m_clientWakeRequests.confirmConnected(clientName);
	// The GUI uses this established connection event to cancel a pending
	// wake and reset backoff, so it must survive restrictive log filters.
	LOG((CLOG_PRINT "client \"%s\" has connected", clientName.c_str()));

	// send configuration options to client
	sendOptions(client);

	// activate screen saver on new client if active on the primary screen
	if (m_activeSaver != NULL) {
		client->screensaver(true);
	}

	// send notification
	Server::ScreenConnectedInfo* info =
		new Server::ScreenConnectedInfo(getName(client));
	m_events->addEvent(Event(m_events->forServer().connected(),
								m_primaryClient->getEventTarget(), info));
}

void
Server::disconnect()
{
	// close all secondary clients
	if (m_clients.size() > 1 || !m_oldClients.empty()) {
		Config emptyConfig(m_events);
		closeClients(emptyConfig);
	}
	else {
		m_events->addEvent(Event(m_events->forServer().disconnected(), this));
	}
}

UInt32
Server::getNumClients() const
{
	return (SInt32)m_clients.size();
}

void
Server::getClients(std::vector<std::string>& list) const
{
	list.clear();
	for (ClientList::const_iterator index = m_clients.begin();
							index != m_clients.end(); ++index) {
		list.push_back(index->first);
	}
}

std::string Server::getName(const BaseClientProxy* client) const
{
    std::string name = m_config->getCanonicalName(client->getName());
	if (name.empty()) {
		name = client->getName();
	}
	return name;
}

UInt32
Server::getActivePrimarySides() const
{
	UInt32 sides = 0;
	if (!isLockedToScreenServer()) {
		if (hasAnyNeighbor(m_primaryClient, kLeft)) {
			sides |= kLeftMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kRight)) {
			sides |= kRightMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kTop)) {
			sides |= kTopMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kBottom)) {
			sides |= kBottomMask;
		}
	}
	return sides;
}

bool
Server::isLockedToScreenServer() const
{
	// locked if scroll-lock is toggled on
	return m_lockedToScreen;
}

bool
Server::isLockedToScreen() const
{
	// locked if we say we're locked
	if (isLockedToScreenServer()) {
		return true;
	}

	// locked if primary says we're locked
	if (m_primaryClient->isLockedToScreen()) {
		return true;
	}

	// not locked
	return false;
}

SInt32
Server::getJumpZoneSize(BaseClientProxy* client) const
{
	if (client == m_primaryClient) {
		return m_primaryClient->getJumpZoneSize();
	}
	else {
		return 0;
	}
}

void
Server::switchScreen(BaseClientProxy* dst,
				SInt32 x, SInt32 y, bool forScreensaver, bool force)
{
	if (!force && !m_switchingEnabled && dst != m_active) {
		return;
	}

	assert(dst != NULL);

#ifndef NDEBUG
	{
		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		assert(x >= dx && y >= dy && x < dx + dw && y < dy + dh);
	}
#endif
	assert(m_active != NULL);

	// resolve the physical displays selected by routing for the log: the
	// display under the cursor on the source screen (m_x, m_y still hold
	// the source position) and the display containing the landing point
	// on the destination screen.  unknown displays append no label, so
	// the existing log text stays byte-identical.
	std::vector<ScreenRect> srcDisplays, dstDisplays;
	std::vector<std::string> srcNames, dstNames;
	m_active->getDisplays(srcDisplays);
	m_active->getDisplayNames(srcNames);
	dst->getDisplays(dstDisplays);
	dst->getDisplayNames(dstNames);

	LOG((CLOG_INFO "switch from \"%s\"%s to \"%s\"%s at %d,%d",
	     getName(m_active).c_str(),
	     barrier::displayLabelSuffix(
	         barrier::displayLabelAt(srcDisplays, srcNames, m_x, m_y)).c_str(),
	     getName(dst).c_str(),
	     barrier::displayLabelSuffix(
	         barrier::displayLabelAt(dstDisplays, dstNames, x, y)).c_str(),
	     x, y));

	// stop waiting to switch
	stopSwitch();

	// record new position
	m_x       = x;
	m_y       = y;
	m_xDelta  = 0;
	m_yDelta  = 0;
	m_xDelta2 = 0;
	m_yDelta2 = 0;

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		// leave active screen
		if (!m_active->leave()) {
			// cannot leave screen
			LOG((CLOG_WARN "can't leave screen"));
			return;
		}

		// update the primary client's clipboards if we're leaving the
		// primary screen.
		if (m_active == m_primaryClient && m_enableClipboard) {
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				ClipboardInfo& clipboard = m_clipboards[id];
				if (clipboard.m_clipboardOwner == getName(m_primaryClient)) {
					onClipboardChanged(m_primaryClient,
						id, clipboard.m_clipboardSeqNum);
				}
			}
		}

		// cut over
		m_active = dst;

		// increment enter sequence number
		++m_seqNum;

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_primaryClient->getToggleMask(),
								forScreensaver);

		if (m_enableClipboard) {
			// send the clipboard data to new active screen
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				m_active->setClipboard(id, &m_clipboards[id].m_clipboard);
			}
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
	}
	else {
		m_active->mouseMove(x, y);
	}
}

void
Server::jumpToScreen(BaseClientProxy* newScreen)
{
	assert(newScreen != NULL);

	// record the current cursor position on the active screen
	m_active->setJumpCursorPos(m_x, m_y);

	// get the last cursor position on the target screen
	SInt32 x, y;
	newScreen->getJumpCursorPos(x, y);

	switchScreen(newScreen, x, y, false);
}

float
Server::mapToFraction(BaseClientProxy* client,
				EDirection dir, SInt32 x, SInt32 y) const
{
	SInt32 sx, sy, sw, sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		return static_cast<float>(y - sy + 0.5f) / static_cast<float>(sh);

	case kTop:
	case kBottom:
		return static_cast<float>(x - sx + 0.5f) / static_cast<float>(sw);

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
	return 0.0f;
}

void
Server::mapToPixel(BaseClientProxy* client,
				EDirection dir, float f, SInt32& x, SInt32& y) const
{
	SInt32 sx, sy, sw, sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		y = static_cast<SInt32>(f * sh) + sy;
		break;

	case kTop:
	case kBottom:
		x = static_cast<SInt32>(f * sw) + sx;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
}

bool
Server::hasAnyNeighbor(BaseClientProxy* client, EDirection dir) const
{
	assert(client != NULL);

	return m_config->hasNeighbor(getName(client), dir);
}

BaseClientProxy*
Server::getNeighbor(BaseClientProxy* src,
				EDirection dir, SInt32& x, SInt32& y,
				std::string* firstDisconnectedTarget) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get source screen name
    std::string srcName = getName(src);
	assert(!srcName.empty());
	LOG((CLOG_DEBUG2 "find neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));

	// convert position to fraction
	float t = mapToFraction(src, dir, x, y);

	// search for the closest neighbor that exists in direction dir
	float tTmp;
	for (;;) {
        std::string dstName(m_config->getNeighbor(srcName, dir, t, &tTmp));

		// if nothing in that direction then return NULL. if the
		// destination is the source then we can make no more
		// progress in this direction.  since we haven't found a
		// connected neighbor we return NULL.
		if (dstName.empty()) {
			LOG((CLOG_DEBUG2 "no neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));
			return NULL;
		}

		// look up neighbor cell.  if the screen is connected and
		// ready then we can stop.
		ClientList::const_iterator index = m_clients.find(dstName);
		if (index != m_clients.end()) {
			LOG((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\" at %f", dstName.c_str(), Config::dirName(dir), srcName.c_str(), t));
			mapToPixel(index->second, dir, tTmp, x, y);
			return index->second;
		}

		// skip over unconnected screen
		if (firstDisconnectedTarget != NULL &&
				firstDisconnectedTarget->empty()) {
			*firstDisconnectedTarget = dstName;
		}
		LOG((CLOG_DEBUG2 "ignored \"%s\" on %s of \"%s\"", dstName.c_str(), Config::dirName(dir), srcName.c_str()));
		srcName = dstName;

		// use position on skipped screen
		t = tTmp;
	}
}

BaseClientProxy*
Server::mapToNeighbor(BaseClientProxy* src,
				EDirection srcSide, SInt32& x, SInt32& y,
				std::string* firstDisconnectedTarget) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	// get the first neighbor
	BaseClientProxy* dst = getNeighbor(
		src, srcSide, x, y, firstDisconnectedTarget);
	if (dst == NULL) {
		return NULL;
	}

	// get the source screen's size
	SInt32 dx, dy, dw, dh;
	BaseClientProxy* lastGoodScreen = src;
	lastGoodScreen->getShape(dx, dy, dw, dh);

	// find destination screen, adjusting x or y (but not both).  the
	// searches are done in a sort of canonical screen space where
	// the upper-left corner is 0,0 for each screen.  we adjust from
	// actual to canonical position on entry to and from canonical to
	// actual on exit from the search.
	switch (srcSide) {
	case kLeft:
		x -= dx;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			x += dw;
			if (x >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(
				lastGoodScreen, srcSide, x, y, firstDisconnectedTarget);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		// L-shaped destination: the left/right edge x depends on y.
		{
			std::vector<ScreenRect> rects;
			lastGoodScreen->getDisplays(rects);
			if (rects.size() > 1) {
				for (std::vector<ScreenRect>::const_iterator i = rects.begin();
						i != rects.end(); ++i) {
					if (y >= i->y && y < i->y + i->h) {
						if (srcSide == kLeft) {
							x = i->x + i->w - 1;
						} else {
							x = i->x;
						}
						break;
					}
				}
			}
		}
		break;

	case kRight:
		x -= dx;
		while (dst != NULL) {
			x -= dw;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (x < dw) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(
				lastGoodScreen, srcSide, x, y, firstDisconnectedTarget);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		{
			std::vector<ScreenRect> rects;
			lastGoodScreen->getDisplays(rects);
			if (rects.size() > 1) {
				for (std::vector<ScreenRect>::const_iterator i = rects.begin();
						i != rects.end(); ++i) {
					if (y >= i->y && y < i->y + i->h) {
						if (srcSide == kLeft) {
							x = i->x + i->w - 1;
						} else {
							x = i->x;
						}
						break;
					}
				}
			}
		}
		break;

	case kTop:
		y -= dy;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			y += dh;
			if (y >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(
				lastGoodScreen, srcSide, x, y, firstDisconnectedTarget);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		// for an L-shaped screen, the top edge is not a single straight
		// line.  adjust y to the top of the display that actually contains x
		// so the cursor lands on the correct monitor instead of the gap.
		{
			std::vector<ScreenRect> rects;
			lastGoodScreen->getDisplays(rects);
			if (rects.size() > 1) {
				for (std::vector<ScreenRect>::const_iterator i = rects.begin();
						i != rects.end(); ++i) {
					if (x >= i->x && x < i->x + i->w) {
						y = i->y;
						break;
					}
				}
			}
		}
		break;

	case kBottom:
		y -= dy;
		while (dst != NULL) {
			y -= dh;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (y < dh) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(
				lastGoodScreen, srcSide, x, y, firstDisconnectedTarget);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		// for an L-shaped screen, the top edge is stepped.  when entering
		// from above, land on the display whose horizontal span contains x.
		{
			std::vector<ScreenRect> rects;
			lastGoodScreen->getDisplays(rects);
			if (rects.size() > 1) {
				for (std::vector<ScreenRect>::const_iterator i = rects.begin();
						i != rects.end(); ++i) {
					if (x >= i->x && x < i->x + i->w) {
						y = i->y;
						break;
					}
				}
			}
		}
		break;


	case kNoDirection:
		assert(0 && "bad direction");
		return NULL;
	}

	// save destination screen
	assert(lastGoodScreen != NULL);
	dst = lastGoodScreen;

	// if entering primary screen then be sure to move in far enough
	// to avoid the jump zone.  if entering a side that doesn't have
	// a neighbor (i.e. an asymmetrical side) then we don't need to
	// move inwards because that side can't provoke a jump.
	avoidJumpZone(dst, srcSide, x, y);

	return dst;
}

void
Server::requestClientWake(const std::string& target)
{
	const std::string canonicalTarget =
		m_config->getCanonicalName(target);
	if (canonicalTarget.empty() ||
			m_clients.find(canonicalTarget) != m_clients.end()) {
		return;
	}

	std::string payload;
	if (!barrier::formatClientWakeRequest(canonicalTarget, payload) ||
			!m_clientWakeRequests.shouldEmit(
				canonicalTarget, m_clientWakeClock.getTime())) {
		return;
	}

	// This is a daemon-to-GUI control message, so it must survive even when
	// the visible log level is stricter than NOTE.
	LOG((CLOG_PRINT "%s", payload.c_str()));
}

void
Server::avoidJumpZone(BaseClientProxy* dst,
				EDirection dir, SInt32& x, SInt32& y) const
{
	// we only need to avoid jump zones on the primary screen
	if (dst != m_primaryClient) {
		return;
	}

    const std::string dstName(getName(dst));
	SInt32 dx, dy, dw, dh;
	dst->getShape(dx, dy, dw, dh);
	float t = mapToFraction(dst, dir, x, y);
	SInt32 z = getJumpZoneSize(dst);

	// move in far enough to avoid the jump zone.  if entering a side
	// that doesn't have a neighbor (i.e. an asymmetrical side) then we
	// don't need to move inwards because that side can't provoke a jump.
	switch (dir) {
	case kLeft:
		if (!m_config->getNeighbor(dstName, kRight, t, NULL).empty() &&
			x > dx + dw - 1 - z)
			x = dx + dw - 1 - z;
		break;

	case kRight:
		if (!m_config->getNeighbor(dstName, kLeft, t, NULL).empty() &&
			x < dx + z)
			x = dx + z;
		break;

	case kTop:
		if (!m_config->getNeighbor(dstName, kBottom, t, NULL).empty() &&
			y > dy + dh - 1 - z)
			y = dy + dh - 1 - z;
		break;

	case kBottom:
		if (!m_config->getNeighbor(dstName, kTop, t, NULL).empty() &&
			y < dy + z)
			y = dy + z;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
	}
}

bool
Server::isSwitchOkay(BaseClientProxy* newScreen,
				EDirection dir, SInt32 x, SInt32 y,
				SInt32 xActive, SInt32 yActive)
{
	LOG((CLOG_DEBUG1 "try to leave \"%s\" on %s", getName(m_active).c_str(), Config::dirName(dir)));

	// is there a neighbor?
	if (newScreen == NULL) {
		// there's no neighbor.  we don't want to switch and we don't
		// want to try to switch later.
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(dir)));
		stopSwitch();
		return false;
	}

	// should we switch or not?
	bool preventSwitch = false;
	bool allowSwitch   = false;

	// note if the switch direction has changed.  save the new
	// direction and screen if so.
	bool isNewDirection  = (dir != m_switchDir);
	if (isNewDirection || m_switchScreen == NULL) {
		m_switchDir    = dir;
		m_switchScreen = newScreen;
	}

	// is this a double tap and do we care?
	if (!allowSwitch && m_switchTwoTapDelay > 0.0) {
		if (isNewDirection ||
			!isSwitchTwoTapStarted() || !shouldSwitchTwoTap()) {
			// tapping a different or new edge or second tap not
			// fast enough.  prepare for second tap.
			preventSwitch = true;
			startSwitchTwoTap();
		}
		else {
			// got second tap
			allowSwitch = true;
		}
	}

	// if waiting before a switch then prepare to switch later
	if (!allowSwitch && m_switchWaitDelay > 0.0) {
		if (isNewDirection || !isSwitchWaitStarted()) {
			startSwitchWait(x, y);
		}
		preventSwitch = true;
	}

	// are we in a locked corner?  first check if screen has the option set
	// and, if not, check the global options.
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(m_active));
	if (options == NULL || options->count(kOptionScreenSwitchCorners) == 0) {
		options = m_config->getOptions("");
	}
	if (options != NULL && options->count(kOptionScreenSwitchCorners) > 0) {
		// get corner mask and size
		Config::ScreenOptions::const_iterator i =
			options->find(kOptionScreenSwitchCorners);
		UInt32 corners = static_cast<UInt32>(i->second);
		i = options->find(kOptionScreenSwitchCornerSize);
		SInt32 size = 0;
		if (i != options->end()) {
			size = i->second;
		}

		// see if we're in a locked corner
		if ((getCorner(m_active, xActive, yActive, size) & corners) != 0) {
			// yep, no switching
			LOG((CLOG_DEBUG1 "locked in corner"));
			preventSwitch = true;
			stopSwitch();
		}
	}

	// ignore if mouse is locked to screen and don't try to switch later
	if (!preventSwitch && isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		preventSwitch = true;
		stopSwitch();
	}

	// check for optional needed modifiers
	KeyModifierMask mods = this->m_primaryClient->getToggleMask();

	if (!preventSwitch && (
			(this->m_switchNeedsShift && ((mods & KeyModifierShift) != KeyModifierShift)) ||
			(this->m_switchNeedsControl && ((mods & KeyModifierControl) != KeyModifierControl)) ||
			(this->m_switchNeedsAlt && ((mods & KeyModifierAlt) != KeyModifierAlt))
		)) {
		LOG((CLOG_DEBUG1 "need modifiers to switch"));
		preventSwitch = true;
		stopSwitch();
	}

	return !preventSwitch;
}

void
Server::noSwitch(SInt32 x, SInt32 y)
{
	armSwitchTwoTap(x, y);
	stopSwitchWait();
}

void
Server::stopSwitch()
{
	if (m_switchScreen != NULL) {
		m_switchScreen = NULL;
		m_switchDir    = kNoDirection;
		stopSwitchTwoTap();
		stopSwitchWait();
	}
}

void
Server::startSwitchTwoTap()
{
	m_switchTwoTapEngaged = true;
	m_switchTwoTapArmed   = false;
	m_switchTwoTapTimer.reset();
	LOG((CLOG_DEBUG1 "waiting for second tap"));
}

void
Server::armSwitchTwoTap(SInt32 x, SInt32 y)
{
	if (m_switchTwoTapEngaged) {
		if (m_switchTwoTapTimer.getTime() > m_switchTwoTapDelay) {
			// second tap took too long.  disengage.
			stopSwitchTwoTap();
		}
		else if (!m_switchTwoTapArmed) {
			// still time for a double tap.  see if we left the tap
			// zone and, if so, arm the two tap.
			SInt32 ax, ay, aw, ah;
			m_active->getShape(ax, ay, aw, ah);
			SInt32 tapZone = m_primaryClient->getJumpZoneSize();
			if (tapZone < m_switchTwoTapZone) {
				tapZone = m_switchTwoTapZone;
			}
			if (x >= ax + tapZone && x < ax + aw - tapZone &&
				y >= ay + tapZone && y < ay + ah - tapZone) {
				// win32 can generate bogus mouse events that appear to
				// move in the opposite direction that the mouse actually
				// moved.  try to ignore that crap here.
				switch (m_switchDir) {
				case kLeft:
					m_switchTwoTapArmed = (m_xDelta > 0 && m_xDelta2 > 0);
					break;

				case kRight:
					m_switchTwoTapArmed = (m_xDelta < 0 && m_xDelta2 < 0);
					break;

				case kTop:
					m_switchTwoTapArmed = (m_yDelta > 0 && m_yDelta2 > 0);
					break;

				case kBottom:
					m_switchTwoTapArmed = (m_yDelta < 0 && m_yDelta2 < 0);
					break;

				default:
					break;
				}
			}
		}
	}
}

void
Server::stopSwitchTwoTap()
{
	m_switchTwoTapEngaged = false;
	m_switchTwoTapArmed   = false;
}

bool
Server::isSwitchTwoTapStarted() const
{
	return m_switchTwoTapEngaged;
}

bool
Server::shouldSwitchTwoTap() const
{
	// this is the second tap if two-tap is armed and this tap
	// came fast enough
	return (m_switchTwoTapArmed &&
			m_switchTwoTapTimer.getTime() <= m_switchTwoTapDelay);
}

void
Server::startSwitchWait(SInt32 x, SInt32 y)
{
	stopSwitchWait();
	m_switchWaitX     = x;
	m_switchWaitY     = y;
	m_switchWaitTimer = m_events->newOneShotTimer(m_switchWaitDelay, this);
	LOG((CLOG_DEBUG1 "waiting to switch"));
}

void
Server::stopSwitchWait()
{
	if (m_switchWaitTimer != NULL) {
		m_events->deleteTimer(m_switchWaitTimer);
		m_switchWaitTimer = NULL;
	}
}

bool
Server::isSwitchWaitStarted() const
{
	return (m_switchWaitTimer != NULL);
}

UInt32
Server::getCorner(BaseClientProxy* client,
				SInt32 x, SInt32 y, SInt32 size) const
{
	assert(client != NULL);

	// get client screen shape
	SInt32 ax, ay, aw, ah;
	client->getShape(ax, ay, aw, ah);

	// check for x,y on the left or right
	SInt32 xSide;
	if (x <= ax) {
		xSide = -1;
	}
	else if (x >= ax + aw - 1) {
		xSide = 1;
	}
	else {
		xSide = 0;
	}

	// check for x,y on the top or bottom
	SInt32 ySide;
	if (y <= ay) {
		ySide = -1;
	}
	else if (y >= ay + ah - 1) {
		ySide = 1;
	}
	else {
		ySide = 0;
	}

	// if against the left or right then check if y is within size
	if (xSide != 0) {
		if (y < ay + size) {
			return (xSide < 0) ? kTopLeftMask : kTopRightMask;
		}
		else if (y >= ay + ah - size) {
			return (xSide < 0) ? kBottomLeftMask : kBottomRightMask;
		}
	}

	// if against the left or right then check if y is within size
	if (ySide != 0) {
		if (x < ax + size) {
			return (ySide < 0) ? kTopLeftMask : kBottomLeftMask;
		}
		else if (x >= ax + aw - size) {
			return (ySide < 0) ? kTopRightMask : kBottomRightMask;
		}
	}

	return kNoCornerMask;
}

void
Server::stopRelativeMoves()
{
	if (m_relativeMoves && m_active != m_primaryClient) {
		// warp to the center of the active client so we know where we are
		SInt32 ax, ay, aw, ah;
		m_active->getShape(ax, ay, aw, ah);
		m_x       = ax + (aw >> 1);
		m_y       = ay + (ah >> 1);
		m_xDelta  = 0;
		m_yDelta  = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;
		LOG((CLOG_DEBUG2 "synchronize move on %s by %d,%d", getName(m_active).c_str(), m_x, m_y));
		m_active->mouseMove(m_x, m_y);
	}
}

void
Server::sendOptions(BaseClientProxy* client) const
{
	OptionsList optionsList;

	// look up options for client
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(client));
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(2 * options->size());
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// look up global options
	options = m_config->getOptions("");
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(optionsList.size() + 2 * options->size());
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// send the options
	client->resetOptions();
	client->setOptions(optionsList);
}

void
Server::processOptions()
{
	const Config::ScreenOptions* options = m_config->getOptions("");
	if (options == NULL) {
		return;
	}

	m_switchNeedsShift = false;		// it seems if I don't add these
	m_switchNeedsControl = false;	// lines, the 'reload config' option
	m_switchNeedsAlt = false;		// doesn't work correct.

	bool newRelativeMoves = m_relativeMoves;
	for (Config::ScreenOptions::const_iterator index = options->begin();
								index != options->end(); ++index) {
		const OptionID id       = index->first;
		const OptionValue value = index->second;
		if (id == kOptionScreenSwitchDelay) {
			m_switchWaitDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchWaitDelay < 0.0) {
				m_switchWaitDelay = 0.0;
			}
			stopSwitchWait();
		}
		else if (id == kOptionScreenSwitchTwoTap) {
			m_switchTwoTapDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchTwoTapDelay < 0.0) {
				m_switchTwoTapDelay = 0.0;
			}
			stopSwitchTwoTap();
		}
		else if (id == kOptionScreenSwitchNeedsControl) {
			m_switchNeedsControl = (value != 0);
		}
		else if (id == kOptionScreenSwitchNeedsShift) {
			m_switchNeedsShift = (value != 0);
		}
		else if (id == kOptionScreenSwitchNeedsAlt) {
			m_switchNeedsAlt = (value != 0);
		}
		else if (id == kOptionRelativeMouseMoves) {
			newRelativeMoves = (value != 0);
		}
		else if (id == kOptionClipboardSharing) {
			m_enableClipboard = (value != 0);

			if (m_enableClipboard == false) {
				LOG((CLOG_NOTE "clipboard sharing is disabled"));
			}
		}
	}
	if (m_relativeMoves && !newRelativeMoves) {
		stopRelativeMoves();
	}
	m_relativeMoves = newRelativeMoves;
}

std::int64_t
Server::displayTopologyMonotonicMs()
{
	const double seconds = ARCH->time();
	if (seconds <= 0.0) {
		return 0;
	}

	const double maximumSeconds =
		static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1000.0;
	if (seconds >= maximumSeconds) {
		return std::numeric_limits<std::int64_t>::max();
	}
	return static_cast<std::int64_t>(seconds * 1000.0);
}

bool
Server::hasDisplayTopologyProfile(
		const barrier::DisplayTopology& topology) const
{
	try {
		const barrier::DisplayTopology normalized = topology.normalized();
		const std::string key = normalized.profileKey();
		Config::TopologyProfileMap::const_iterator profile =
			m_config->topologyProfiles().find(key);
		return profile != m_config->topologyProfiles().end() &&
			profile->second.topology.normalized().canonicalIdentity() ==
				normalized.canonicalIdentity();
	}
	catch (const std::invalid_argument&) {
		return false;
	}
}

DisplayTopologyDecision
Server::updateDisplayTopology(
		const barrier::DisplayTopology& topology,
		std::int64_t monotonicMs,
		DisplayTopologyUpdateSource source)
{
	try {
		m_currentDisplayTopology = topology.normalized();
	}
	catch (const std::invalid_argument&) {
		m_currentDisplayTopology = barrier::DisplayTopology();
	}

	const bool profileKnown =
		hasDisplayTopologyProfile(m_currentDisplayTopology);
	const DisplayTopologyDecision decision =
		source == DisplayTopologyUpdateSource::ReconfigurationSnapshot
			? m_displayTopologyStateMachine.observeReconfigurationSnapshot(
				m_currentDisplayTopology, profileKnown, monotonicMs)
			: m_displayTopologyStateMachine.observe(
				m_currentDisplayTopology, profileKnown, monotonicMs);
	applyDisplayTopologyDecision(decision);
	armDisplayTopologyTimer(decision.nextDeadlineMs, monotonicMs);
	return decision;
}

Config::DisplayRectMap
Server::currentLiveDisplayRects() const
{
	Config::DisplayRectMap liveDisplayRects;
	for (ClientList::const_iterator client = m_clients.begin();
		 client != m_clients.end(); ++client) {
		std::vector<ScreenRect> displays;
		client->second->getDisplays(displays);
		liveDisplayRects[client->first] = displays;
	}
	return liveDisplayRects;
}

DisplayTopologyDecision
Server::beginDisplayTopologyReconfiguration(std::int64_t monotonicMs)
{
	const DisplayTopologyDecision decision =
		m_displayTopologyStateMachine.beginReconfiguration(monotonicMs);
	applyDisplayTopologyDecision(decision);
	armDisplayTopologyTimer(decision.nextDeadlineMs, monotonicMs);
	return decision;
}

DisplayTopologyDecision
Server::updateDisplayTopologyDeadline(std::int64_t monotonicMs)
{
	const DisplayTopologyDecision decision =
		m_displayTopologyStateMachine.onDeadline(monotonicMs);
	applyDisplayTopologyDecision(decision);
	armDisplayTopologyTimer(decision.nextDeadlineMs, monotonicMs);
	return decision;
}

void
Server::applyDisplayTopologyDecision(
		const DisplayTopologyDecision& decision)
{
	DisplayTopologyDecision effectiveDecision = decision;
	bool profileActivated = false;
	if (decision.state == DisplayTopologyState::StableKnown) {
		profileActivated =
			m_config->selectTopology(
				m_currentDisplayTopology, currentLiveDisplayRects()) ==
				Config::TopologySelectionResult::Known;
		if (!profileActivated) {
			// A saved profile can still be unusable with a connected client's
			// current DDIS. Keep the safe empty map and surface Layout required.
			effectiveDecision.state = DisplayTopologyState::StableUnknown;
			effectiveDecision.switchingEnabled = false;
		}
	}
	else {
		m_config->selectTopology(barrier::DisplayTopology());
	}
	m_switchingEnabled =
		effectiveDecision.switchingEnabled && profileActivated;
	m_primaryClient->reconfigure(getActivePrimarySides());

	if (!m_switchingEnabled && m_active != m_primaryClient) {
		SInt32 x, y, width, height;
		m_primaryClient->getShape(x, y, width, height);
		if (width > 0 && height > 0) {
			switchScreen(m_primaryClient, x + width / 2, y + height / 2,
						 false, true);
		}
		else {
			stopSwitch();
			m_active = m_primaryClient;
			m_x = x;
			m_y = y;
		}
	}

	if (effectiveDecision.disconnectClients) {
		disconnect();
	}

	emitDisplayTopologyStatus(effectiveDecision);
}

void
Server::armDisplayTopologyTimer(
		std::int64_t deadlineMs, std::int64_t monotonicMs)
{
	stopDisplayTopologyTimer();
	if (deadlineMs < 0) {
		return;
	}

	const std::int64_t remainingMs =
		deadlineMs > monotonicMs ? deadlineMs - monotonicMs : 0;
	m_displayTopologyTimer = m_events->newOneShotTimer(
		static_cast<double>(remainingMs) / 1000.0,
		&m_displayTopologyStateMachine);
}

void
Server::stopDisplayTopologyTimer()
{
	if (m_displayTopologyTimer != NULL) {
		m_events->deleteTimer(m_displayTopologyTimer);
		m_displayTopologyTimer = NULL;
	}
}

void
Server::emitDisplayTopologyStatus(
		const DisplayTopologyDecision& decision)
{
	const char* state = "Unavailable";
	switch (decision.state) {
	case DisplayTopologyState::Reconfiguring:
		state = "Reconfiguring";
		break;
	case DisplayTopologyState::StableKnown:
		state = "StableKnown";
		break;
	case DisplayTopologyState::StableUnknown:
		state = "StableUnknown";
		break;
	case DisplayTopologyState::NoDisplayGrace:
		state = "NoDisplayGrace";
		break;
	case DisplayTopologyState::Unavailable:
		break;
	}

	std::string key("-");
	std::string displays;
	if (!m_currentDisplayTopology.empty()) {
		try {
			key = m_currentDisplayTopology.profileKey();
			displays = m_currentDisplayTopology.canonicalIdentity();
			static const size_t kMaximumDisplayStatusBytes = 8192;
			if (displays.size() > kMaximumDisplayStatusBytes) {
				displays = "oversize";
			}
		}
		catch (const std::invalid_argument&) {
			key = "-";
			displays.clear();
		}
	}

	std::ostringstream stream;
	stream << "BARRIER_TOPOLOGY\tstate=" << state
		   << "\tkey=" << key
		   << "\tdisplays=" << displays;
	const std::string status = stream.str();
	if (status != m_lastDisplayTopologyStatus) {
		m_lastDisplayTopologyStatus = status;
		LOG((CLOG_NOTE "%s", status.c_str()));
	}
}

void
Server::handleDisplayTopologyTimer(const Event&, void*)
{
	updateDisplayTopologyDeadline(displayTopologyMonotonicMs());
}

void
Server::handleDisplayReconfigurationStarted(const Event&, void* vclient)
{
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	if (client != m_primaryClient || m_clientSet.count(client) == 0) {
		return;
	}

	LOG((CLOG_DEBUG "primary display reconfiguration started"));
	beginDisplayTopologyReconfiguration(displayTopologyMonotonicMs());
}

void
Server::handleShapeChanged(const Event&, void* vclient)
{
	// ignore events from unknown clients
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(client) == 0) {
		return;
	}

	LOG((CLOG_DEBUG "screen \"%s\" shape changed", getName(client).c_str()));
	if (client == m_primaryClient) {
		updateDisplayTopology(m_primaryClient->getDisplayTopology(),
							 displayTopologyMonotonicMs(),
							 DisplayTopologyUpdateSource::ReconfigurationSnapshot);
	}
	else if (!m_currentDisplayTopology.empty()) {
		// A connected client's DDIS is live geometry for the active profile.
		// Re-select atomically so links use it without restarting either peer.
		updateDisplayTopology(m_currentDisplayTopology,
							 displayTopologyMonotonicMs());
	}

	// update jump coordinate
	SInt32 x, y;
	client->getCursorPos(x, y);
	client->setJumpCursorPos(x, y);

	// update the mouse coordinates
	if (client == m_active) {
		m_x = x;
		m_y = y;
	}

	// handle resolution change to primary screen
	if (client == m_primaryClient) {
		if (client == m_active) {
			onMouseMovePrimary(m_x, m_y);
		}
		else {
			onMouseMoveSecondary(0, 0);
		}
	}
}

void
Server::handleClipboardGrabbed(const Event& event, void* vclient)
{
	if (!m_enableClipboard) {
		return;
	}

	// ignore events from unknown clients
	BaseClientProxy* grabber = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(grabber) == 0) {
		return;
	}
	const IScreen::ClipboardInfo* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());

	// ignore grab if sequence number is old.  always allow primary
	// screen to grab.
	ClipboardInfo& clipboard = m_clipboards[info->m_id];
	if (grabber != m_primaryClient &&
		info->m_sequenceNumber < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" grab of clipboard %d", getName(grabber).c_str(), info->m_id));
		return;
	}

	// mark screen as owning clipboard
	LOG((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", getName(grabber).c_str(), info->m_id, clipboard.m_clipboardOwner.c_str()));
	clipboard.m_clipboardOwner  = getName(grabber);
	clipboard.m_clipboardSeqNum = info->m_sequenceNumber;

	// clear the clipboard data (since it's not known at this point)
	if (clipboard.m_clipboard.open(0)) {
		clipboard.m_clipboard.empty();
		clipboard.m_clipboard.close();
	}
	clipboard.m_clipboardData = clipboard.m_clipboard.marshall();

	// tell all other screens to take ownership of clipboard.  tell the
	// grabber that it's clipboard isn't dirty.
	for (ClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		if (client == grabber) {
			client->setClipboardDirty(info->m_id, false);
		}
		else {
			client->grabClipboard(info->m_id);
		}
	}
}

void
Server::handleClipboardChanged(const Event& event, void* vclient)
{
	// ignore events from unknown clients
	BaseClientProxy* sender = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(sender) == 0) {
		return;
	}
	const IScreen::ClipboardInfo* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());
	onClipboardChanged(sender, info->m_id, info->m_sequenceNumber);
}

void
Server::handleKeyDownEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyDown(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyUpEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		 static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyUp(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyRepeatEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyRepeat(info->m_key, info->m_mask, info->m_count, info->m_button);
}

void
Server::handleButtonDownEvent(const Event& event, void*)
{
	IPlatformScreen::ButtonInfo* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseDown(info->m_button);
}

void
Server::handleButtonUpEvent(const Event& event, void*)
{
	IPlatformScreen::ButtonInfo* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseUp(info->m_button);
}

void
Server::handleMotionPrimaryEvent(const Event& event, void*)
{
	IPlatformScreen::MotionInfo* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMovePrimary(info->m_x, info->m_y);
}

void
Server::handleMotionSecondaryEvent(const Event& event, void*)
{
	IPlatformScreen::MotionInfo* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMoveSecondary(info->m_x, info->m_y);
}

void
Server::handleWheelEvent(const Event& event, void*)
{
	IPlatformScreen::WheelInfo* info =
		static_cast<IPlatformScreen::WheelInfo*>(event.getData());
	onMouseWheel(info->m_xDelta, info->m_yDelta);
}

void
Server::handleScreensaverActivatedEvent(const Event&, void*)
{
	onScreensaver(true);
}

void
Server::handleScreensaverDeactivatedEvent(const Event&, void*)
{
	onScreensaver(false);
}

void
Server::handleSwitchWaitTimeout(const Event&, void*)
{
	// ignore if mouse is locked to screen
	if (isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		stopSwitch();
		return;
	}

	// switch screen
	switchScreen(m_switchScreen, m_switchWaitX, m_switchWaitY, false);
}

void
Server::handleClientDisconnected(const Event&, void* vclient)
{
	// client has disconnected.  it might be an old client or an
	// active client.  we don't care so just handle it both ways.
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	removeActiveClient(client);
	removeOldClient(client);

	delete client;
}

void
Server::handleClientCloseTimeout(const Event&, void* vclient)
{
	// client took too long to disconnect.  just dump it.
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	LOG((CLOG_NOTE "forced disconnection of client \"%s\"", getName(client).c_str()));
	removeOldClient(client);

	delete client;
}

void
Server::handleSwitchToScreenEvent(const Event& event, void*)
{
	SwitchToScreenInfo* info =
		static_cast<SwitchToScreenInfo*>(event.getData());

	const std::string target = m_config->getCanonicalName(info->m_screen);
	ClientList::const_iterator index = m_clients.find(target);
	if (index == m_clients.end()) {
		LOG((CLOG_DEBUG1 "screen \"%s\" not active", info->m_screen));
		requestClientWake(target);
	}
	else {
		jumpToScreen(index->second);
	}
}

void
Server::handleToggleScreenEvent(const Event& event, void*)
{
  std::string current = getName(m_active);
  ClientList::const_iterator index = m_clients.find(current);
  if (index == m_clients.end()) {
    LOG((CLOG_DEBUG1 "screen \"%s\" not active", current.c_str()));
  }
  else {
    ++index;
    if (index == m_clients.end()) {
      index = m_clients.begin();
    }
    jumpToScreen(index->second);
  }
}


void
Server::handleSwitchInDirectionEvent(const Event& event, void*)
{
	SwitchInDirectionInfo* info =
		static_cast<SwitchInDirectionInfo*>(event.getData());

	// jump to screen in chosen direction from center of this screen
	SInt32 x = m_x, y = m_y;
	std::string disconnectedTarget;
	BaseClientProxy* newScreen =
		getNeighbor(m_active, info->m_direction, x, y,
			&disconnectedTarget);
	requestClientWake(disconnectedTarget);
	if (newScreen == NULL) {
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(info->m_direction)));
	}
	else {
		jumpToScreen(newScreen);
	}
}

void
Server::handleKeyboardBroadcastEvent(const Event& event, void*)
{
	KeyboardBroadcastInfo* info = (KeyboardBroadcastInfo*)event.getData();

	// choose new state
	bool newState;
	switch (info->m_state) {
	case KeyboardBroadcastInfo::kOff:
		newState = false;
		break;

	default:
	case KeyboardBroadcastInfo::kOn:
		newState = true;
		break;

	case KeyboardBroadcastInfo::kToggle:
		newState = !m_keyboardBroadcasting;
		break;
	}

	// enter new state
	if (newState != m_keyboardBroadcasting ||
		info->m_screens != m_keyboardBroadcastingScreens) {
		m_keyboardBroadcasting        = newState;
		m_keyboardBroadcastingScreens = info->m_screens;
		LOG((CLOG_DEBUG "keyboard broadcasting %s: %s", m_keyboardBroadcasting ? "on" : "off", m_keyboardBroadcastingScreens.c_str()));
	}
}

void
Server::handleLockCursorToScreenEvent(const Event& event, void*)
{
	LockCursorToScreenInfo* info = (LockCursorToScreenInfo*)event.getData();

	// choose new state
	bool newState;
	switch (info->m_state) {
	case LockCursorToScreenInfo::kOff:
		newState = false;
		break;

	default:
	case LockCursorToScreenInfo::kOn:
		newState = true;
		break;

	case LockCursorToScreenInfo::kToggle:
		newState = !m_lockedToScreen;
		break;
	}

	// enter new state
	if (newState != m_lockedToScreen) {
		m_lockedToScreen = newState;
		LOG((CLOG_NOTE "cursor %s current screen", m_lockedToScreen ? "locked to" : "unlocked from"));

		m_primaryClient->reconfigure(getActivePrimarySides());
		if (!isLockedToScreenServer()) {
			stopRelativeMoves();
		}
	}
}

void
Server::handleFakeInputBeginEvent(const Event&, void*)
{
	m_primaryClient->fakeInputBegin();
}

void
Server::handleFakeInputEndEvent(const Event&, void*)
{
	m_primaryClient->fakeInputEnd();
}

void
Server::handleFileChunkSendingEvent(const Event& event, void*)
{
	onFileChunkSending(event.getData());
}

void
Server::handleFileRecieveCompletedEvent(const Event& event, void*)
{
	onFileRecieveCompleted();
}

void
Server::onClipboardChanged(BaseClientProxy* sender,
				ClipboardID id, UInt32 seqNum)
{
	ClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", getName(sender).c_str(), id));
		return;
	}

	// should be the expected client
	assert(sender == m_clients.find(clipboard.m_clipboardOwner)->second);

	// get data
	sender->getClipboard(id, &clipboard.m_clipboard);

	// ignore if data hasn't changed
    std::string data = clipboard.m_clipboard.marshall();
	if (data == clipboard.m_clipboardData) {
		LOG((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// got new data
	LOG((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	clipboard.m_clipboardData = data;

	// tell all clients except the sender that the clipboard is dirty
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->setClipboardDirty(id, client != sender);
	}

	// send the new clipboard to the active screen
	m_active->setClipboard(id, &clipboard.m_clipboard);
}

void
Server::onScreensaver(bool activated)
{
	LOG((CLOG_DEBUG "onScreenSaver %s", activated ? "activated" : "deactivated"));

	if (activated) {
		// save current screen and position
		m_activeSaver = m_active;
		m_xSaver      = m_x;
		m_ySaver      = m_y;

		// jump to primary screen
		if (m_active != m_primaryClient) {
			switchScreen(m_primaryClient, 0, 0, true);
		}
	}
	else {
		// jump back to previous screen and position.  we must check
		// that the position is still valid since the screen may have
		// changed resolutions while the screen saver was running.
		if (m_activeSaver != NULL && m_activeSaver != m_primaryClient) {
			// check position
			BaseClientProxy* screen = m_activeSaver;
			SInt32 x, y, w, h;
			screen->getShape(x, y, w, h);
			SInt32 zoneSize = getJumpZoneSize(screen);
			if (m_xSaver < x + zoneSize) {
				m_xSaver = x + zoneSize;
			}
			else if (m_xSaver >= x + w - zoneSize) {
				m_xSaver = x + w - zoneSize - 1;
			}
			if (m_ySaver < y + zoneSize) {
				m_ySaver = y + zoneSize;
			}
			else if (m_ySaver >= y + h - zoneSize) {
				m_ySaver = y + h - zoneSize - 1;
			}

			// jump
			switchScreen(screen, m_xSaver, m_ySaver, false);
		}

		// reset state
		m_activeSaver = NULL;
	}

	// send message to all clients
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->screensaver(activated);
	}
}

void
Server::onKeyDown(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyDown id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		m_active->keyDown(id, mask, button);
	}
	else {
		if (!screens && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyDown(id, mask, button);
			}
		}
	}
}

void
Server::onKeyUp(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyUp id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		m_active->keyUp(id, mask, button);
	}
	else {
		if (!screens && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyUp(id, mask, button);
			}
		}
	}
}

void
Server::onKeyRepeat(KeyID id, KeyModifierMask mask,
				SInt32 count, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyRepeat id=%d mask=0x%04x count=%d button=0x%04x", id, mask, count, button));
	assert(m_active != NULL);

	// relay
	m_active->keyRepeat(id, mask, count, button);
}

void
Server::onMouseDown(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseDown id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseDown(id);

	// reset this variable back to default value true
	m_waitDragInfoThread = true;
}

void
Server::onMouseUp(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseUp id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseUp(id);

	if (m_ignoreFileTransfer) {
		m_ignoreFileTransfer = false;
		return;
	}

	if (m_args.m_enableDragDrop) {
		if (!m_screen->isOnScreen()) {
            std::string& file = m_screen->getDraggingFilename();
			if (!file.empty()) {
				sendFileToClient(file.c_str());
			}
		}

		// always clear dragging filename
		m_screen->clearDraggingFilename();
	}
}

bool
Server::onMouseMovePrimary(SInt32 x, SInt32 y)
{
	LOG((CLOG_DEBUG4 "onMouseMovePrimary %d,%d", x, y));

	// mouse move on primary (server's) screen
	if (m_active != m_primaryClient) {
		// stale event -- we're actually on a secondary screen
		return false;
	}

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = x - m_x;
	m_yDelta  = y - m_y;

	// save position
	m_x       = x;
	m_y       = y;

	// get screen shape and per-display rectangles
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);
	std::vector<ScreenRect> displays;
	m_active->getDisplays(displays);
	SInt32 zoneSize = getJumpZoneSize(m_active);

	// clamp position to screen
	SInt32 xc = x, yc = y;
	if (xc < ax + zoneSize) {
		xc = ax;
	}
	else if (xc >= ax + aw - zoneSize) {
		xc = ax + aw - 1;
	}
	if (yc < ay + zoneSize) {
		yc = ay;
	}
	else if (yc >= ay + ah - zoneSize) {
		yc = ay + ah - 1;
	}

	// find the display the cursor is on so an L-shaped (non-rectangular)
	// layout switches on the real monitor edge rather than the bounding box.
	const ScreenRect* disp = NULL;
	for (std::vector<ScreenRect>::const_iterator i = displays.begin();
			i != displays.end(); ++i) {
		if (x >= i->x && x < i->x + i->w && y >= i->y && y < i->y + i->h) {
			disp = &(*i);
			break;
		}
	}
	SInt32 sx = (disp != NULL) ? disp->x : ax;
	SInt32 sy = (disp != NULL) ? disp->y : ay;
	SInt32 sw = (disp != NULL) ? disp->w : aw;
	SInt32 sh = (disp != NULL) ? disp->h : ah;

	// see if we should change screens
	// when the cursor is in a corner, there may be a screen either
	// horizontally or vertically.  check both directions.
	EDirection dirh = kNoDirection, dirv = kNoDirection;
	SInt32 xh = x, yv = y;
	// mapToNeighbor() performs its multi-screen traversal in the source
	// screen union frame.  Use the crossed physical display to preserve
	// overshoot, but anchor that overshoot at the union edge that traversal
	// expects.  Without the union width/height on right/bottom crossings,
	// a normal right-edge jump passes x=0 into mapToNeighbor(), which lands
	// one source width left of the target and immediately bounces back.
	if (x < sx + zoneSize) {
		xh  = barrier::canonicalEdgeCoordinate(
			kLeft, ScreenRect{ax, ay, aw, ah}, ScreenRect{sx, sy, sw, sh},
			x, y, zoneSize);
		dirh = kLeft;
	}
	else if (x >= sx + sw - zoneSize) {
		xh  = barrier::canonicalEdgeCoordinate(
			kRight, ScreenRect{ax, ay, aw, ah}, ScreenRect{sx, sy, sw, sh},
			x, y, zoneSize);
		dirh = kRight;
	}
	if (y < sy + zoneSize) {
		yv  = barrier::canonicalEdgeCoordinate(
			kTop, ScreenRect{ax, ay, aw, ah}, ScreenRect{sx, sy, sw, sh},
			x, y, zoneSize);
		dirv = kTop;
	}
	else if (y >= sy + sh - zoneSize) {
		yv  = barrier::canonicalEdgeCoordinate(
			kBottom, ScreenRect{ax, ay, aw, ah}, ScreenRect{sx, sy, sw, sh},
			x, y, zoneSize);
		dirv = kBottom;
	}

	// The fraction coordinates (y for a horizontal crossing, x for a
	// vertical one) deliberately stay in the screen union frame: the
	// configured link intervals are encoded against that same union, so
	// remapping them relative to the crossed display would make partial
	// edges (e.g. LG's top edge, which spans only 1920/3000 of the
	// union) unreachable beyond their per-display fraction.  The xh/yv
	// offsets above already canonicalize the landing position relative
	// to the display actually crossed.
	if (dirh == kNoDirection && dirv == kNoDirection) {
		// still on local screen
		noSwitch(x, y);
		return false;
	}

	// check both horizontally and vertically
	EDirection dirs[] = {dirh, dirv};
	SInt32 xs[] = {xh, x}, ys[] = {y, yv};
	for (int i = 0; i < 2; ++i) {
		EDirection dir = dirs[i];
		if (dir == kNoDirection) {
			continue;
		}
		x = xs[i], y = ys[i];

		// Decide who owns this physical edge before any neighbor lookup:
		// a boundary shared with another display of the same screen is
		// LocalOS (the OS moves the cursor natively), an external edge
		// without a configured cross-host link at this coordinate clamps
		// at the display's own edge, and only a linked external edge
		// reaches mapToNeighbor().  A non-Remote direction is skipped so
		// a corner can still switch through its other direction.
		if (disp != NULL) {
			std::vector<Config::Interval> intervals;
			const std::string activeName(getName(m_active));
			for (Config::link_const_iterator it = m_config->beginNeighbor(activeName);
					it != m_config->endNeighbor(activeName); ++it) {
				if (it->first.getSide() == dir) {
					intervals.push_back(it->first.getInterval());
				}
			}
			switch (barrier::classifyDisplayBoundary(
						*disp, dir, displays, intervals, m_x, m_y)) {
			case barrier::BoundaryOwner::LocalOS:
				LOG((CLOG_DEBUG1 "same-host boundary on %s of \"%s\" left to the OS",
					Config::dirName(dir), activeName.c_str()));
				// Clear any pending switch: a wait armed at a previously
				// linked edge must not fire while the OS moves the cursor
				// across this same-host boundary.
				stopSwitch();
				continue;
			case barrier::BoundaryOwner::Clamp: {
				LOG((CLOG_DEBUG1 "unlinked edge on %s of \"%s\" clamps",
					Config::dirName(dir), activeName.c_str()));
				SInt32 clampX = m_x, clampY = m_y;
				switch (dir) {
				case kLeft:
					clampX = disp->x;
					break;
				case kRight:
					clampX = disp->x + disp->w - 1;
					break;
				case kTop:
					clampY = disp->y;
					break;
				case kBottom:
					clampY = disp->y + disp->h - 1;
					break;
				default:
					break;
				}
				noSwitch(clampX, clampY);
				continue;
			}
			case barrier::BoundaryOwner::Remote:
				break;
			}
		}

		// get jump destination
		std::string disconnectedTarget;
		BaseClientProxy* newScreen = mapToNeighbor(
			m_active, dir, x, y, &disconnectedTarget);
		requestClientWake(disconnectedTarget);

		// should we switch or not?
		if (isSwitchOkay(newScreen, dir, x, y, xc, yc)) {
			if (m_args.m_enableDragDrop
				&& m_screen->isDraggingStarted()
				&& m_active != newScreen
				&& m_waitDragInfoThread) {
				if (m_sendDragInfoThread == NULL) {
                    m_sendDragInfoThread = new Thread([this, newScreen]()
                                                      { send_drag_info_thread(newScreen); });
				}

				return false;
			}

			// switch screen
			switchScreen(newScreen, x, y, false);
			m_waitDragInfoThread = true;
			return true;
		}
	}

	return false;
}

void Server::send_drag_info_thread(BaseClientProxy* newScreen)
{
	m_dragFileList.clear();
    std::string& dragFileList = m_screen->getDraggingFilename();
	if (!dragFileList.empty()) {
		DragInformation di;
		di.setFilename(dragFileList);
		m_dragFileList.push_back(di);
	}

#if defined(__APPLE__)
	// on mac it seems that after faking a LMB up, system would signal back
	// to barrier a mouse up event, which doesn't happen on windows. as a
	// result, barrier would send dragging file to client twice. This variable
	// is used to ignore the first file sending.
	m_ignoreFileTransfer = true;
#endif

	// send drag file info to client if there is any
	if (m_dragFileList.size() > 0) {
		sendDragInfo(newScreen);
		m_dragFileList.clear();
	}
	m_waitDragInfoThread = false;
	m_sendDragInfoThread = NULL;
}

void
Server::sendDragInfo(BaseClientProxy* newScreen)
{
    std::string infoString;
	UInt32 fileCount = DragInformation::setupDragInfo(m_dragFileList, infoString);

	if (fileCount > 0) {
		char* info = NULL;
		size_t size = infoString.size();
		info = new char[size];
		memcpy(info, infoString.c_str(), size);

		LOG((CLOG_DEBUG2 "sending drag information to client"));
		LOG((CLOG_DEBUG3 "dragging file list: %s", info));
		LOG((CLOG_DEBUG3 "dragging file list string size: %i", size));
		newScreen->sendDragInfo(fileCount, info, size);
	}
}

void
Server::onMouseMoveSecondary(SInt32 dx, SInt32 dy)
{
	LOG((CLOG_DEBUG2 "onMouseMoveSecondary %+d,%+d", dx, dy));

	// mouse move on secondary (client's) screen
	assert(m_active != NULL);
	if (m_active == m_primaryClient) {
		// stale event -- we're actually on the primary screen
		return;
	}

	// if doing relative motion on secondary screens and we're locked
	// to the screen (which activates relative moves) then send a
	// relative mouse motion.  when we're doing this we pretend as if
	// the mouse isn't actually moving because we're expecting some
	// program on the secondary screen to warp the mouse on us, so we
	// have no idea where it really is.
	if (m_relativeMoves && isLockedToScreenServer()) {
		LOG((CLOG_DEBUG2 "relative move on %s by %d,%d", getName(m_active).c_str(), dx, dy));
		m_active->mouseRelativeMove(dx, dy);
		return;
	}

	// save old position
	const SInt32 xOld = m_x;
	const SInt32 yOld = m_y;

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = dx;
	m_yDelta  = dy;

	// accumulate motion
	m_x      += dx;
	m_y      += dy;

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);

	// find direction of neighbor and get the neighbor
	bool jump = true;
	BaseClientProxy* newScreen;
	do {
		// clamp position to screen
		SInt32 xc = m_x, yc = m_y;
		if (xc < ax) {
			xc = ax;
		}
		else if (xc >= ax + aw) {
			xc = ax + aw - 1;
		}
		if (yc < ay) {
			yc = ay;
		}
		else if (yc >= ay + ah) {
			yc = ay + ah - 1;
		}

		EDirection dir;
		if (m_x < ax) {
			dir = kLeft;
		}
		else if (m_x > ax + aw - 1) {
			dir = kRight;
		}
		else if (m_y < ay) {
			dir = kTop;
		}
		else if (m_y > ay + ah - 1) {
			dir = kBottom;
		}
		else {
			// we haven't left the screen
			newScreen = m_active;
			jump      = false;

			// if waiting and mouse is not on the border we're waiting
			// on then stop waiting.  also if it's not on the border
			// then arm the double tap.
			if (m_switchScreen != NULL) {
				bool clearWait;
				SInt32 zoneSize = m_primaryClient->getJumpZoneSize();
				switch (m_switchDir) {
				case kLeft:
					clearWait = (m_x >= ax + zoneSize);
					break;

				case kRight:
					clearWait = (m_x <= ax + aw - 1 - zoneSize);
					break;

				case kTop:
					clearWait = (m_y >= ay + zoneSize);
					break;

				case kBottom:
					clearWait = (m_y <= ay + ah - 1 + zoneSize);
					break;

				default:
					clearWait = false;
					break;
				}
				if (clearWait) {
					// still on local screen
					noSwitch(m_x, m_y);
				}
			}

			// skip rest of block
			break;
		}

		// try to switch screen.  get the neighbor.
		std::string disconnectedTarget;
		newScreen = mapToNeighbor(
			m_active, dir, m_x, m_y, &disconnectedTarget);
		requestClientWake(disconnectedTarget);

		// see if we should switch
		if (!isSwitchOkay(newScreen, dir, m_x, m_y, xc, yc)) {
			newScreen = m_active;
			jump      = false;
		}
	} while (false);

	if (jump) {
		if (m_sendFileThread != NULL) {
			StreamChunker::interruptFile();
			m_sendFileThread = NULL;
		}

		SInt32 newX = m_x;
		SInt32 newY = m_y;

		// switch screens
		switchScreen(newScreen, newX, newY, false);
	}
	else {
		// same screen.  clamp mouse to edge.
		m_x = xOld + dx;
		m_y = yOld + dy;
		if (m_x < ax) {
			m_x = ax;
			LOG((CLOG_DEBUG2 "clamp to left of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_x > ax + aw - 1) {
			m_x = ax + aw - 1;
			LOG((CLOG_DEBUG2 "clamp to right of \"%s\"", getName(m_active).c_str()));
		}
		if (m_y < ay) {
			m_y = ay;
			LOG((CLOG_DEBUG2 "clamp to top of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_y > ay + ah - 1) {
			m_y = ay + ah - 1;
			LOG((CLOG_DEBUG2 "clamp to bottom of \"%s\"", getName(m_active).c_str()));
		}

		// warp cursor if it moved.
		if (m_x != xOld || m_y != yOld) {
			LOG((CLOG_DEBUG2 "move on %s to %d,%d", getName(m_active).c_str(), m_x, m_y));
			m_active->mouseMove(m_x, m_y);
		}
	}
}

void
Server::onMouseWheel(SInt32 xDelta, SInt32 yDelta)
{
	LOG((CLOG_DEBUG1 "onMouseWheel %+d,%+d", xDelta, yDelta));
	assert(m_active != NULL);

	// relay
	m_active->mouseWheel(xDelta, yDelta);
}

void
Server::onFileChunkSending(const void* data)
{
	FileChunk* chunk = static_cast<FileChunk*>(const_cast<void*>(data));

	LOG((CLOG_DEBUG1 "sending file chunk"));
	assert(m_active != NULL);

	// relay
	m_active->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1], chunk->m_dataSize);
}

void
Server::onFileRecieveCompleted()
{
	if (isReceivedFileSizeValid()) {
        m_writeToDropDirThread = new Thread([this]() { write_to_drop_dir_thread(); });
	}
}

void Server::write_to_drop_dir_thread()
{
	LOG((CLOG_DEBUG "starting write to drop dir thread"));

	while (m_screen->isFakeDraggingStarted()) {
		ARCH->sleep(.1f);
	}

	DropHelper::writeToDir(m_screen->getDropTarget(), m_fakeDragFileList,
					m_receivedFileData);
}

bool
Server::addClient(BaseClientProxy* client)
{
    std::string name = getName(client);
	if (m_clients.count(name) != 0) {
		return false;
	}

	// add event handlers
	m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleShapeChanged, client));
	if (client == m_primaryClient) {
		m_events->adoptHandler(
			m_events->forIScreen().displayReconfigurationStarted(),
			client->getEventTarget(),
			new TMethodEventJob<Server>(
				this, &Server::handleDisplayReconfigurationStarted, client));
	}
	m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardGrabbed, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardChanged, client));

	// add to list
	m_clientSet.insert(client);
	m_clients.insert(std::make_pair(name, client));

	// initialize client data
	SInt32 x, y;
	client->getCursorPos(x, y);
	client->setJumpCursorPos(x, y);

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());

	if (client != m_primaryClient && !m_currentDisplayTopology.empty()) {
		// Apply DDIS already received during the handshake. If DDIS arrives
		// after adoption, ClientProxy1_0 emits shapeChanged for the same path.
		updateDisplayTopology(m_currentDisplayTopology,
						  displayTopologyMonotonicMs());
	}

	return true;
}

bool
Server::removeClient(BaseClientProxy* client)
{
	// return false if not in list
	ClientSet::iterator i = m_clientSet.find(client);
	if (i == m_clientSet.end()) {
		return false;
	}

	// remove event handlers
	m_events->removeHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget());
	if (client == m_primaryClient) {
		m_events->removeHandler(
			m_events->forIScreen().displayReconfigurationStarted(),
			client->getEventTarget());
	}
	m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget());

	// remove from list
	m_clients.erase(getName(client));
	m_clientSet.erase(i);

	return true;
}

void
Server::closeClient(BaseClientProxy* client, const char* msg)
{
	assert(client != m_primaryClient);
	assert(msg != NULL);

	// send message to client.  this message should cause the client
	// to disconnect.  we add this client to the closed client list
	// and install a timer to remove the client if it doesn't respond
	// quickly enough.  we also remove the client from the active
	// client list since we're not going to listen to it anymore.
	// note that this method also works on clients that are not in
	// the m_clients list.  adoptClient() may call us with such a
	// client.
	LOG((CLOG_NOTE "disconnecting client \"%s\"", getName(client).c_str()));

	// send message
	// FIXME -- avoid type cast (kinda hard, though)
	((ClientProxy*)client)->close(msg);

	// install timer.  wait timeout seconds for client to close.
	double timeout = 5.0;
	EventQueueTimer* timer = m_events->newOneShotTimer(timeout, NULL);
	m_events->adoptHandler(Event::kTimer, timer,
							new TMethodEventJob<Server>(this,
								&Server::handleClientCloseTimeout, client));

	// move client to closing list
	removeClient(client);
	m_oldClients.insert(std::make_pair(client, timer));

	// if this client is the active screen then we have to
	// jump off of it
	forceLeaveClient(client);
}

void
Server::closeClients(const Config& config)
{
	// collect the clients that are connected but are being dropped
	// from the configuration (or who's canonical name is changing).
	typedef std::set<BaseClientProxy*> RemovedClients;
	RemovedClients removed;
	for (ClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		if (!config.isCanonicalName(index->first)) {
			removed.insert(index->second);
		}
	}

	// don't close the primary client
	removed.erase(m_primaryClient);

	// now close them.  we collect the list then close in two steps
	// because closeClient() modifies the collection we iterate over.
	for (RemovedClients::iterator index = removed.begin();
								index != removed.end(); ++index) {
		closeClient(*index, kMsgCClose);
	}
}

void
Server::removeActiveClient(BaseClientProxy* client)
{
	if (removeClient(client)) {
		forceLeaveClient(client);
		if (!m_currentDisplayTopology.empty()) {
			// Drop the departed client's live DDIS override and atomically
			// rebuild from the saved profile. Without this, a transient invalid
			// DDIS could leave Layout required after that client was gone.
			updateDisplayTopology(m_currentDisplayTopology,
							 displayTopologyMonotonicMs());
		}
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::removeOldClient(BaseClientProxy* client)
{
	OldClients::iterator i = m_oldClients.find(client);
	if (i != m_oldClients.end()) {
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		m_events->removeHandler(Event::kTimer, i->second);
		m_events->deleteTimer(i->second);
		m_oldClients.erase(i);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::forceLeaveClient(BaseClientProxy* client)
{
	BaseClientProxy* active =
		(m_activeSaver != NULL) ? m_activeSaver : m_active;
	if (active == client) {
		// record new position (center of primary screen)
		m_primaryClient->getCursorCenter(m_x, m_y);

		// stop waiting to switch to this client
		if (active == m_switchScreen) {
			stopSwitch();
		}

		// don't notify active screen since it has probably already
		// disconnected.
		LOG((CLOG_INFO "jump from \"%s\" to \"%s\" at %d,%d", getName(active).c_str(), getName(m_primaryClient).c_str(), m_x, m_y));

		// cut over
		m_active = m_primaryClient;

		// enter new screen (unless we already have because of the
		// screen saver)
		if (m_activeSaver == NULL) {
			m_primaryClient->enter(m_x, m_y, m_seqNum,
								m_primaryClient->getToggleMask(), false);
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
	}

	// if this screen had the cursor when the screen saver activated
	// then we can't switch back to it when the screen saver
	// deactivates.
	if (m_activeSaver == client) {
		m_activeSaver = NULL;
	}

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());
}


//
// Server::ClipboardInfo
//

Server::ClipboardInfo::ClipboardInfo() :
	m_clipboard(),
	m_clipboardData(),
	m_clipboardOwner(),
	m_clipboardSeqNum(0)
{
	// do nothing
}


//
// Server::LockCursorToScreenInfo
//

Server::LockCursorToScreenInfo*
Server::LockCursorToScreenInfo::alloc(State state)
{
	LockCursorToScreenInfo* info =
		(LockCursorToScreenInfo*)malloc(sizeof(LockCursorToScreenInfo));
	info->m_state = state;
	return info;
}


//
// Server::SwitchToScreenInfo
//

Server::SwitchToScreenInfo*
Server::SwitchToScreenInfo::alloc(const std::string& screen)
{
	SwitchToScreenInfo* info =
		(SwitchToScreenInfo*)malloc(sizeof(SwitchToScreenInfo) +
								screen.size());
	strcpy(info->m_screen, screen.c_str());
	return info;
}


//
// Server::SwitchInDirectionInfo
//

Server::SwitchInDirectionInfo*
Server::SwitchInDirectionInfo::alloc(EDirection direction)
{
	SwitchInDirectionInfo* info =
		(SwitchInDirectionInfo*)malloc(sizeof(SwitchInDirectionInfo));
	info->m_direction = direction;
	return info;
}

//
// Server::KeyboardBroadcastInfo
//

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state)
{
	KeyboardBroadcastInfo* info =
		(KeyboardBroadcastInfo*)malloc(sizeof(KeyboardBroadcastInfo));
	info->m_state      = state;
	info->m_screens[0] = '\0';
	return info;
}

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state, const std::string& screens)
{
	KeyboardBroadcastInfo* info =
		(KeyboardBroadcastInfo*)malloc(sizeof(KeyboardBroadcastInfo) +
								screens.size());
	info->m_state = state;
	strcpy(info->m_screens, screens.c_str());
	return info;
}

bool
Server::isReceivedFileSizeValid()
{
	return m_expectedFileSize == m_receivedFileData.size();
}

void
Server::sendFileToClient(const char* filename)
{
	if (m_sendFileThread != NULL) {
		StreamChunker::interruptFile();
	}

    m_sendFileThread = new Thread([this, filename]() { send_file_thread(filename); });
}

void Server::send_file_thread(const char* filename)
{
	try {
		LOG((CLOG_DEBUG "sending file to client, filename=%s", filename));
		StreamChunker::sendFile(filename, m_events, this);
	}
	catch (std::runtime_error &error) {
		LOG((CLOG_ERR "failed sending file chunks, error: %s", error.what()));
	}

	m_sendFileThread = NULL;
}

void
Server::dragInfoReceived(UInt32 fileNum, std::string content)
{
	if (!m_args.m_enableDragDrop) {
		LOG((CLOG_DEBUG "drag drop not enabled, ignoring drag info."));
		return;
	}

	DragInformation::parseDragInfo(m_fakeDragFileList, fileNum, content);

	m_screen->startDraggingFiles(m_fakeDragFileList);
}
