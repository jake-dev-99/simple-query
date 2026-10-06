#pragma once

#if defined(HAS_LIBEBOOK) || defined(HAS_LIBECAL)
/** Purpose: Read one optional string field from a projected row.
 * @param row is a borrowed native record.
 * @param key identifies the field.
 * @returns The field text, or an empty string when absent.
 * @throws Nothing. */
std::string ProjectedString(FlValue* row, const char* key) {
  return AsString(FindValue(row, key)).value_or("");
}
#endif

#ifdef HAS_LIBEBOOK
/** Purpose: Prove real EDS contacts expose their native revision.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestContactProjectionUsesRevision() {
  constexpr char kVCard[] =
      "BEGIN:VCARD\r\n"
      "VERSION:3.0\r\n"
      "UID:contact-1\r\n"
      "FN:Ada Example\r\n"
      "ORG:Simple Zen\r\n"
      "TEL:+15550000001\r\n"
      "EMAIL:ada@example.test\r\n"
      "REV:20261005T120000Z\r\n"
      "END:VCARD\r\n";
  g_autoptr(EContact) contact = e_contact_new_from_vcard(kVCard);
  g_assert_nonnull(contact);
  auto row = ProjectContactRecord(contact);
  const std::string id = ProjectedString(row.get(), "id");
  const std::string updated_at = ProjectedString(row.get(), "updatedAt");
  g_assert_cmpstr(id.c_str(), ==, "contact-1");
  g_assert_cmpstr(updated_at.c_str(), ==, "20261005T120000Z");
  const auto initial_signature = SnapshotSignature(row.get());
  g_assert_false(initial_signature.error.has_value());
  e_contact_set(contact, E_CONTACT_FULL_NAME,
                const_cast<gchar*>("Ada Changed"));
  auto changed_row = ProjectContactRecord(contact);
  const auto changed_signature = SnapshotSignature(changed_row.get());
  g_assert_false(changed_signature.error.has_value());
  g_assert_true(initial_signature.value != changed_signature.value);
}
#endif

#ifdef HAS_LIBECAL
/** Purpose: Prove real EDS events use LAST-MODIFIED, not recurrence identity.
 * @param None. @returns Nothing.
 * @throws Nothing. */
void TestCalendarProjectionUsesModificationTime() {
  constexpr char kEvent[] =
      "BEGIN:VEVENT\r\n"
      "UID:event-1\r\n"
      "SUMMARY:Planning\r\n"
      "DTSTART:20261005T120000Z\r\n"
      "DTEND:20261005T130000Z\r\n"
      "DTSTAMP:20261005T110000Z\r\n"
      "LAST-MODIFIED:20261005T113000Z\r\n"
      "RECURRENCE-ID:20261012T120000Z\r\n"
      "END:VEVENT\r\n";
  ICalComponent* component = i_cal_component_new_from_string(kEvent);
  g_assert_nonnull(component);
  auto row = ProjectCalendarRecord(component, "calendar-1");
  const std::string id = ProjectedString(row.get(), "id");
  const std::string updated_at = ProjectedString(row.get(), "updatedAt");
  g_assert_cmpstr(id.c_str(), ==, "event-1");
  g_assert_cmpstr(updated_at.c_str(), ==, "20261005T113000Z");
  const auto initial_signature = SnapshotSignature(row.get());
  g_assert_false(initial_signature.error.has_value());
  i_cal_component_set_summary(component, "Planning changed");
  auto changed_row = ProjectCalendarRecord(component, "calendar-1");
  const auto changed_signature = SnapshotSignature(changed_row.get());
  g_assert_false(changed_signature.error.has_value());
  g_assert_true(initial_signature.value != changed_signature.value);
  g_object_unref(component);
}
#endif
