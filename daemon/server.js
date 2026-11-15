"use strict";

const path = require("path");
const fs = require("fs");
require("dotenv").config();

const admin = require("firebase-admin");
const nodemailer = require("nodemailer");
const {
  parseExcludedEmails,
  filterRecipientEmails,
  shouldProcessEvent,
  buildAlertMailOptions
} = require("./lib/alertHelpers");

// --- 1. Configuration Validation ---
const REQUIRED_ENV_VARS = [
  "FIREBASE_DATABASE_URL",
  "GMAIL_USER",
  "GMAIL_APP_PASSWORD"
];

for (const key of REQUIRED_ENV_VARS) {
  if (!process.env[key] || process.env[key].trim() === "") {
    console.error(`[Config Error] Missing required environment variable: ${key}`);
    process.exit(1);
  }
}

const serviceAccountPath = path.resolve(
  __dirname,
  process.env.FIREBASE_SERVICE_ACCOUNT_PATH || "./serviceAccountKey.json"
);

if (!fs.existsSync(serviceAccountPath)) {
  console.error(`[Config Error] Firebase service account key not found at: ${serviceAccountPath}`);
  process.exit(1);
}

const serviceAccount = require(serviceAccountPath);
const excludedEmails = parseExcludedEmails(process.env.EXCLUDED_EMAILS);

// --- 2. Initialize Firebase Admin & SMTP Transport ---
admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
  databaseURL: process.env.FIREBASE_DATABASE_URL
});

const db = admin.database();
const auth = admin.auth();
const eventsRef = db.ref("events");

const mailTransport = nodemailer.createTransport({
  service: "gmail",
  auth: {
    user: process.env.GMAIL_USER,
    pass: process.env.GMAIL_APP_PASSWORD
  }
});

/**
 * Fetches all Firebase Auth users across paginated results.
 * @returns {Promise<Array<import("firebase-admin").auth.UserRecord>>}
 */
async function listAllUsers() {
  const users = [];
  let nextPageToken;

  do {
    const result = await auth.listUsers(1000, nextPageToken);
    users.push(...result.users);
    nextPageToken = result.pageToken;
  } while (nextPageToken);

  return users;
}

// --- 3. Event Listener Setup ---
const serverStartEpoch = Math.floor(Date.now() / 1000);
const processedEventIds = new Set();
const eventsQuery = eventsRef.orderByChild("epoch").startAt(serverStartEpoch);

async function startDaemon() {
  try {
    await mailTransport.verify();
    console.log("[SMTP] Gmail transport verified and ready.");
  } catch (err) {
    console.error("[SMTP Error] Failed to verify Gmail credentials:", err.message);
    process.exit(1);
  }

  console.log(`[Daemon] Connected to Firebase RTDB (${process.env.FIREBASE_DATABASE_URL})`);
  console.log(`[Daemon] Listening for events from UTC epoch ${serverStartEpoch} (${new Date(serverStartEpoch * 1000).toISOString()})...`);

  eventsQuery.on("child_added", async (snapshot) => {
    const eventId = snapshot.key;
    const event = snapshot.val();

    if (!shouldProcessEvent(eventId, event, serverStartEpoch, processedEventIds)) {
      return;
    }

    processedEventIds.add(eventId);
    const eventType = event.type;
    const timestamp = event.timestamp || new Date().toISOString();

    console.log(`[Alert] New intrusion detected -> ID: ${eventId}, Sensor: ${eventType}, Time: ${timestamp}`);

    try {
      const allUsers = await listAllUsers();
      const recipients = filterRecipientEmails(allUsers, excludedEmails);

      if (recipients.length === 0) {
        console.log("[Alert] No eligible recipients found. Skipping email notification.");
        return;
      }

      console.log(`[Alert] Sending notification to: ${recipients.join(", ")}`);
      const mailOptions = buildAlertMailOptions({
        senderEmail: process.env.GMAIL_USER,
        recipients,
        eventType,
        timestamp
      });

      await mailTransport.sendMail(mailOptions);
      console.log("[Alert] Notification email sent successfully.");
    } catch (err) {
      console.error(`[Alert Error] Failed to process event ${eventId}:`, err);
    }
  });
}

// --- 4. Graceful Shutdown ---
async function shutdown(signal) {
  console.log(`\n[Daemon] Received ${signal}. Shutting down cleanly...`);
  eventsQuery.off();
  await admin.app().delete();
  process.exit(0);
}

process.on("SIGINT", () => shutdown("SIGINT"));
process.on("SIGTERM", () => shutdown("SIGTERM"));

startDaemon();
