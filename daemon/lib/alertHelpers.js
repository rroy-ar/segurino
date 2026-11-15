"use strict";

/**
 * Escapes HTML special characters to prevent markup injection in alert emails.
 * @param {string} value
 * @returns {string}
 */
function escapeHtml(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

/**
 * Parses a comma-separated list of excluded emails into a normalized lowercase Set.
 * @param {string | undefined} rawList
 * @returns {Set<string>}
 */
function parseExcludedEmails(rawList) {
  if (!rawList) {
    return new Set();
  }
  return new Set(
    rawList
      .split(",")
      .map((email) => email.trim().toLowerCase())
      .filter(Boolean)
  );
}

/**
 * Filters Firebase Auth user records to active users with valid, non-excluded emails.
 * @param {Array<{ email?: string, disabled?: boolean }>} users
 * @param {Set<string>} excludedEmails
 * @returns {string[]}
 */
function filterRecipientEmails(users, excludedEmails) {
  const recipients = [];
  for (const user of users) {
    if (!user || user.disabled || !user.email) {
      continue;
    }
    const normalized = user.email.trim().toLowerCase();
    if (!excludedEmails.has(normalized)) {
      recipients.push(user.email.trim());
    }
  }
  return recipients;
}

/**
 * Resolves an event's Unix timestamp in seconds from `epoch` or `timestamp`.
 * Returns null if neither field can be parsed.
 * @param {Record<string, unknown> | null | undefined} event
 * @returns {number | null}
 */
function resolveEventEpochSeconds(event) {
  if (!event || typeof event !== "object") {
    return null;
  }
  if (typeof event.epoch === "number" && Number.isFinite(event.epoch)) {
    return Math.floor(event.epoch);
  }
  if (typeof event.timestamp === "string" && event.timestamp.trim() !== "") {
    const raw = event.timestamp.trim();
    const isoCandidate = raw.includes("T") ? raw : raw.replace(" ", "T") + "Z";
    const parsedMs = Date.parse(isoCandidate);
    if (!Number.isNaN(parsedMs)) {
      return Math.floor(parsedMs / 1000);
    }
  }
  return null;
}

/**
 * Determines whether an event should trigger an alert email.
 * @param {string} eventId
 * @param {Record<string, unknown> | null | undefined} event
 * @param {number} serverStartEpochSeconds
 * @param {Set<string>} processedIds
 * @returns {boolean}
 */
function shouldProcessEvent(eventId, event, serverStartEpochSeconds, processedIds) {
  if (!eventId || processedIds.has(eventId)) {
    return false;
  }
  if (!event || typeof event.type !== "string" || event.type.trim() === "") {
    return false;
  }
  const eventEpoch = resolveEventEpochSeconds(event);
  if (eventEpoch === null) {
    return false;
  }
  return eventEpoch >= serverStartEpochSeconds;
}

/**
 * Builds the Nodemailer payload for an intrusion alert email.
 * @param {{ senderEmail: string, recipients: string[], eventType: string, timestamp: string }} params
 */
function buildAlertMailOptions({ senderEmail, recipients, eventType, timestamp }) {
  const safeType = escapeHtml(eventType);
  const safeTimestamp = escapeHtml(timestamp);

  return {
    from: `"Segurino Alarm System" <${senderEmail}>`,
    to: recipients.join(", "),
    subject: `INTRUSION ALERT: ${eventType}`,
    html: `
      <div style="font-family: Arial, sans-serif; color: #1f2937; max-width: 520px;">
        <h2 style="color: #dc2626; margin-bottom: 12px;">Security Intrusion Detected</h2>
        <p>Your Segurino alarm system detected a confirmed intrusion event:</p>
        <ul style="line-height: 1.7;">
          <li><strong>Sensor Triggered:</strong> ${safeType}</li>
          <li><strong>Timestamp (UTC):</strong> ${safeTimestamp}</li>
        </ul>
        <p style="font-size: 0.85em; color: #6b7280;">
          You are receiving this email because your account is registered in the Segurino control panel.
        </p>
      </div>
    `
  };
}

module.exports = {
  escapeHtml,
  parseExcludedEmails,
  filterRecipientEmails,
  resolveEventEpochSeconds,
  shouldProcessEvent,
  buildAlertMailOptions
};
