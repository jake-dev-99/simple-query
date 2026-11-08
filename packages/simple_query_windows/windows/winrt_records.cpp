#include "winrt_records.h"

// Declares RPC_E_CHANGED_MODE and E_ACCESSDENIED; C++/WinRT includes no
// Windows SDK headers.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <winrt/Windows.ApplicationModel.Appointments.h>
#include <winrt/Windows.ApplicationModel.Contacts.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "boundary_formats.h"

namespace simple_query_windows {

namespace {

using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

// Purpose: Name one Windows Runtime store for its errors and denials.
struct StoreSpec {
  // The store's name in an error, such as "contacts".
  const char* backend;
  // The packaged-app capability that grants access, such as "contacts".
  const char* capability;
  // The Windows Settings privacy page that turns access on, such as "Contacts".
  const char* settings_page;
};

constexpr StoreSpec kContactsStore{
    .backend = "contacts", .capability = "contacts", .settings_page = "Contacts"};
constexpr StoreSpec kCalendarStore{
    .backend = "calendar", .capability = "appointments", .settings_page = "Calendar"};

// Purpose: Give the outcome of a store read the user has not allowed.
// @param spec names the store.
// @returns An error that says how to allow access, naming the capability.
// @throws Nothing.
RowsResult DeniedResult(const StoreSpec& spec) {
  RowsResult result;
  result.error = std::string("simple_query: windows ") + spec.backend +
                 " access is denied. Turn on " + spec.settings_page +
                 " access for this app in Windows Settings > Privacy & security.";
  result.denied_capability = spec.capability;
  return result;
}

// Purpose: Give the outcome of a read that stopped before it finished.
// @returns An outcome carrying kReadStoppedError.
// @throws Nothing.
RowsResult StoppedResult() {
  RowsResult result;
  result.error = kReadStoppedError;
  return result;
}

// Purpose: Convert a Windows Runtime time to the system clock.
// @param value is the Windows Runtime time.
// @returns The same instant on the system clock.
// @throws Nothing.
std::chrono::system_clock::time_point TimeFromWinRt(
    winrt::Windows::Foundation::DateTime value) {
  return TimeFromWindowsTicks(value.time_since_epoch().count());
}

// Purpose: Name one read of a Windows Runtime store.
template <typename Open, typename Read>
struct StoreRead {
  StoreSpec spec;
  // Opens the store; gives nothing when the read stopped, and a null store
  // when the user turned access off.
  Open open;
  // Reads the open store.
  Read read;
};

// Purpose: Open a store in a Windows Runtime apartment, read it, and turn
// a stop, a denial or a failure into the read's outcome.
// @param request names the store, how to open it and how to read it.
// @returns The outcome the read gives, kReadStoppedError, a denial naming
// the capability, or an error when the store fails.
// @throws Nothing; store failures are returned in error.
template <typename Open, typename Read>
RowsResult ReadStore(StoreRead<Open, Read> request) {
  const StoreSpec& spec = request.spec;
  try {
    // Joined first, so every store object below is released before the
    // thread leaves the apartment.
    const WinRtApartmentScope apartment;
    const auto store = request.open();
    if (!store.has_value()) {
      return StoppedResult();
    }
    if (!*store) {
      return DeniedResult(spec);
    }
    return request.read(*store);
  } catch (const winrt::hresult_error& error) {
    // A packaged app without the capability gets E_ACCESSDENIED rather
    // than a null store.
    if (static_cast<int32_t>(error.code()) == E_ACCESSDENIED) {
      return DeniedResult(spec);
    }
    RowsResult result;
    result.error = std::string("simple_query: windows ") + spec.backend +
                   " backend unavailable - " + winrt::to_string(error.message());
    return result;
  }
}

// Purpose: Build the query row for one contact.
// @param contact is the contact.
// @returns The row, or nothing for a contact without an id.
// @throws winrt::hresult_error when a contact field cannot be read.
std::optional<EncodableMap> ContactRow(
    const winrt::Windows::ApplicationModel::Contacts::Contact& contact) {
  std::string id = winrt::to_string(contact.Id());
  if (id.empty()) {
    return std::nullopt;
  }

  std::string display_name = winrt::to_string(contact.DisplayName());
  if (display_name.empty()) {
    display_name = winrt::to_string(contact.FirstName());
    const auto last_name = winrt::to_string(contact.LastName());
    if (!last_name.empty()) {
      if (!display_name.empty()) {
        display_name += " ";
      }
      display_name += last_name;
    }
  }

  EncodableList phones;
  for (const auto& phone : contact.Phones()) {
    phones.push_back(EncodableValue(winrt::to_string(phone.Number())));
  }

  EncodableList emails;
  for (const auto& email : contact.Emails()) {
    emails.push_back(EncodableValue(winrt::to_string(email.Address())));
  }

  EncodableMap row;
  row[EncodableValue("id")] = EncodableValue(id);
  row[EncodableValue("displayName")] = EncodableValue(display_name);
  row[EncodableValue("phones")] = EncodableValue(phones);
  row[EncodableValue("emails")] = EncodableValue(emails);

  if (contact.JobInfo().Size() > 0) {
    const auto company = winrt::to_string(contact.JobInfo().GetAt(0).CompanyName());
    if (!company.empty()) {
      row[EncodableValue("organization")] = EncodableValue(company);
    }
  }
  return row;
}

// Purpose: Build the query row for one appointment. No updatedAt is sent:
// the store keeps no modification time, and the field is optional.
// @param appointment is the appointment.
// @param calendar_id is the id of the calendar that holds it.
// @returns The row.
// @throws winrt::hresult_error when an appointment field cannot be read.
EncodableMap AppointmentRow(
    const winrt::Windows::ApplicationModel::Appointments::Appointment& appointment,
    const std::string& calendar_id) {
  const auto start_time = appointment.StartTime();
  const auto start = TimeFromWinRt(start_time);
  const auto end = TimeFromWinRt(start_time + appointment.Duration());

  std::string appointment_id = winrt::to_string(appointment.LocalId());
  if (appointment_id.empty()) {
    const int64_t start_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(start.time_since_epoch())
            .count();
    appointment_id = calendar_id + ":" + std::to_string(start_ms);
  }

  EncodableMap row;
  row[EncodableValue("id")] = EncodableValue(appointment_id);
  row[EncodableValue("title")] = EncodableValue(winrt::to_string(appointment.Subject()));
  row[EncodableValue("startAt")] = EncodableValue(FormatIsoUtc(start));
  row[EncodableValue("endAt")] = EncodableValue(FormatIsoUtc(end));
  row[EncodableValue("isAllDay")] = EncodableValue(appointment.AllDay());
  row[EncodableValue("calendarId")] = EncodableValue(calendar_id);
  return row;
}

// Purpose: Open the contact store for reading.
// @param stop_requested ends the wait early; empty means never.
// @returns The store, or nothing when the read stopped. A null store means
// the user turned contacts access off.
// @throws winrt::hresult_error when the store cannot be opened.
std::optional<winrt::Windows::ApplicationModel::Contacts::ContactStore> OpenContactStore(
    const StopRequested& stop_requested) {
  using namespace winrt::Windows::ApplicationModel::Contacts;
  return AwaitUnlessStopped(
      ContactManager::RequestStoreAsync(ContactStoreAccessType::AllContactsReadOnly),
      stop_requested);
}

// Purpose: Open the appointment store for reading.
// @param stop_requested ends the wait early; empty means never.
// @returns The store, or nothing when the read stopped. A null store means
// the user turned calendar access off.
// @throws winrt::hresult_error when the store cannot be opened.
std::optional<winrt::Windows::ApplicationModel::Appointments::AppointmentStore>
OpenAppointmentStore(const StopRequested& stop_requested) {
  using namespace winrt::Windows::ApplicationModel::Appointments;
  return AwaitUnlessStopped(
      AppointmentManager::RequestStoreAsync(AppointmentStoreAccessType::AllCalendarsReadOnly),
      stop_requested);
}

}  // namespace

// Purpose: Join the multithreaded apartment unless the thread is already in
// a single-threaded one.
// @returns Nothing.
// @throws winrt::hresult_error when the apartment cannot be joined for any
// other reason.
WinRtApartmentScope::WinRtApartmentScope() {
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    joined_ = true;
  } catch (const winrt::hresult_error& error) {
    // RPC_E_CHANGED_MODE means the thread already joined a single-threaded
    // apartment, such as the platform thread; store calls still work there.
    if (static_cast<int32_t>(error.code()) != RPC_E_CHANGED_MODE) {
      throw;
    }
  }
}

// Purpose: Leave the apartment this object joined, if it joined one.
// @returns Nothing.
// @throws Nothing.
WinRtApartmentScope::~WinRtApartmentScope() {
  if (joined_) {
    winrt::uninit_apartment();
  }
}

// Purpose: List the user's contacts as query rows.
// @param stop_requested ends the read early, cancelling any store call in
// flight; empty means never.
// @returns The contact rows, or an error when the read failed, stopped or
// was denied. A denial also names the "contacts" capability.
// @throws Nothing; store failures are returned in error.
RowsResult ListContactRecords(const StopRequested& stop_requested) {
  using winrt::Windows::ApplicationModel::Contacts::ContactStore;
  return ReadStore(StoreRead{
      .spec = kContactsStore,
      .open = [&]() { return OpenContactStore(stop_requested); },
      .read = [&](const ContactStore& store) -> RowsResult {
        const auto contacts = AwaitUnlessStopped(store.FindContactsAsync(), stop_requested);
        if (!contacts.has_value()) {
          return StoppedResult();
        }
        RowsResult result;
        for (const auto& contact : *contacts) {
          if (IsStopRequested(stop_requested)) {
            return StoppedResult();
          }
          auto row = ContactRow(contact);
          if (row.has_value()) {
            result.rows.push_back(std::move(*row));
          }
        }
        return result;
      }});
}

// Purpose: List the appointments from a year back to a year ahead as rows.
// Times are ISO 8601 UTC, as on the other platforms.
// @param stop_requested ends the read early, cancelling any store call in
// flight; empty means never.
// @returns The appointment rows, or an error when the read failed, stopped
// or was denied. A denial also names the "appointments" capability.
// @throws Nothing; store failures are returned in error.
RowsResult ListCalendarRecords(const StopRequested& stop_requested) {
  using winrt::Windows::ApplicationModel::Appointments::AppointmentStore;
  using winrt::Windows::Foundation::TimeSpan;
  return ReadStore(StoreRead{
      .spec = kCalendarStore,
      .open = [&]() { return OpenAppointmentStore(stop_requested); },
      .read = [&](const AppointmentStore& store) -> RowsResult {
        const auto calendars =
            AwaitUnlessStopped(store.FindAppointmentCalendarsAsync(), stop_requested);
        if (!calendars.has_value()) {
          return StoppedResult();
        }
        // C++/WinRT projects TimeSpan as a std::chrono::duration, so it is
        // converted directly rather than through the ABI Duration field.
        const auto lookback = std::chrono::duration_cast<TimeSpan>(std::chrono::hours(24 * 365));
        const auto start = winrt::clock::now() - lookback;
        const auto range = std::chrono::duration_cast<TimeSpan>(std::chrono::hours(24 * 365 * 2));

        RowsResult result;
        for (const auto& calendar : *calendars) {
          const auto appointments =
              AwaitUnlessStopped(calendar.FindAppointmentsAsync(start, range), stop_requested);
          if (!appointments.has_value()) {
            return StoppedResult();
          }
          const std::string calendar_id = winrt::to_string(calendar.LocalId());
          for (const auto& appointment : *appointments) {
            if (IsStopRequested(stop_requested)) {
              return StoppedResult();
            }
            result.rows.push_back(AppointmentRow(appointment, calendar_id));
          }
        }
        return result;
      }});
}

// Purpose: List the user's contact lists.
// @param stop_requested ends the read early; empty means never.
// @returns One row per contact list, with its id and name, or an error as
// ListContactRecords gives one.
// @throws Nothing; store failures are returned in error.
RowsResult ListContactStores(const StopRequested& stop_requested) {
  using winrt::Windows::ApplicationModel::Contacts::ContactStore;
  return ReadStore(StoreRead{
      .spec = kContactsStore,
      .open = [&]() { return OpenContactStore(stop_requested); },
      .read = [&](const ContactStore& store) -> RowsResult {
        const auto lists = AwaitUnlessStopped(store.FindContactListsAsync(), stop_requested);
        if (!lists.has_value()) {
          return StoppedResult();
        }
        RowsResult result;
        for (const auto& list : *lists) {
          EncodableMap row;
          row[EncodableValue("id")] = EncodableValue(winrt::to_string(list.Id()));
          row[EncodableValue("name")] = EncodableValue(winrt::to_string(list.DisplayName()));
          result.rows.push_back(std::move(row));
        }
        return result;
      }});
}

// Purpose: List the user's appointment calendars.
// @param stop_requested ends the read early; empty means never.
// @returns One row per calendar, with its id and title, or an error as
// ListCalendarRecords gives one.
// @throws Nothing; store failures are returned in error.
RowsResult ListCalendarStores(const StopRequested& stop_requested) {
  using winrt::Windows::ApplicationModel::Appointments::AppointmentStore;
  return ReadStore(StoreRead{
      .spec = kCalendarStore,
      .open = [&]() { return OpenAppointmentStore(stop_requested); },
      .read = [&](const AppointmentStore& store) -> RowsResult {
        const auto calendars =
            AwaitUnlessStopped(store.FindAppointmentCalendarsAsync(), stop_requested);
        if (!calendars.has_value()) {
          return StoppedResult();
        }
        RowsResult result;
        for (const auto& calendar : *calendars) {
          EncodableMap row;
          row[EncodableValue("id")] = EncodableValue(winrt::to_string(calendar.LocalId()));
          row[EncodableValue("title")] =
              EncodableValue(winrt::to_string(calendar.DisplayName()));
          result.rows.push_back(std::move(row));
        }
        return result;
      }});
}

}  // namespace simple_query_windows
