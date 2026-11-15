"use strict";

const { describe, it } = require("node:test");
const assert = require("node:assert/strict");
const {
  escapeHtml,
  parseExcludedEmails,
  filterRecipientEmails,
  resolveEventEpochSeconds,
  shouldProcessEvent,
  buildAlertMailOptions
} = require("../lib/alertHelpers");

describe("escapeHtml", () => {
  it("escapes HTML special characters", () => {
    assert.equal(
      escapeHtml('<script>alert("xss") & \'test\'</script>'),
      "&lt;script&gt;alert(&quot;xss&quot;) &amp; &#39;test&#39;&lt;/script&gt;"
    );
  });
});

describe("parseExcludedEmails & filterRecipientEmails", () => {
  it("normalizes excluded emails and filters disabled, missing, and excluded accounts", () => {
    const excluded = parseExcludedEmails(" ESP@arqui.com , server@arqui.com ");
    const users = [
      { email: "esp@arqui.com", disabled: false },
      { email: "server@arqui.com", disabled: false },
      { email: "owner@example.com", disabled: false },
      { email: "disabled@example.com", disabled: true },
      { email: undefined, disabled: false }
    ];

    assert.deepEqual(filterRecipientEmails(users, excluded), ["owner@example.com"]);
  });
});

describe("resolveEventEpochSeconds & shouldProcessEvent", () => {
  it("accepts numeric epoch timestamps at or after server start", () => {
    const processed = new Set();
    const startEpoch = 1760000000;

    assert.equal(
      shouldProcessEvent("evt-1", { type: "PIR", epoch: 1760000005 }, startEpoch, processed),
      true
    );
    assert.equal(
      shouldProcessEvent("evt-old", { type: "PIR", epoch: 1759999999 }, startEpoch, processed),
      false
    );
  });

  it("parses ISO-8601 UTC timestamp strings when numeric epoch is absent", () => {
    const epoch = resolveEventEpochSeconds({ timestamp: "2025-10-12T20:52:00Z" });
    assert.equal(epoch, Math.floor(Date.parse("2025-10-12T20:52:00Z") / 1000));
  });

  it("rejects duplicate event IDs or missing sensor types", () => {
    const processed = new Set(["evt-dup"]);
    assert.equal(
      shouldProcessEvent("evt-dup", { type: "REED", epoch: 1760000010 }, 1760000000, processed),
      false
    );
    assert.equal(
      shouldProcessEvent("evt-bad", { type: "", epoch: 1760000010 }, 1760000000, processed),
      false
    );
  });
});

describe("buildAlertMailOptions", () => {
  it("formats subject and escapes HTML in body", () => {
    const mail = buildAlertMailOptions({
      senderEmail: "alerts@example.com",
      recipients: ["a@example.com", "b@example.com"],
      eventType: "PIR<test>",
      timestamp: "2025-10-12T20:52:00Z"
    });

    assert.equal(mail.to, "a@example.com, b@example.com");
    assert.equal(mail.subject, "INTRUSION ALERT: PIR<test>");
    assert.match(mail.html, /PIR&lt;test&gt;/);
    assert.match(mail.html, /2025-10-12T20:52:00Z/);
  });
});
