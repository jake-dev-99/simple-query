import FlutterMacOS
import Foundation
import Contacts
import EventKit

public class SimpleQueryMacosPlugin: NSObject, FlutterPlugin, NativeQueryHostApi {
  private let flutterApi: NativeQueryFlutterApi
  private var observers: [String: Timer] = [:]
  private var snapshots: [String: [String: Int64]] = [:]
  private var openHandles: [String: String] = [:]

  init(binaryMessenger: FlutterBinaryMessenger) {
    self.flutterApi = NativeQueryFlutterApi(binaryMessenger: binaryMessenger)
    super.init()
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    let instance = SimpleQueryMacosPlugin(binaryMessenger: registrar.messenger)
    NativeQueryHostApiSetup.setUp(
      binaryMessenger: registrar.messenger,
      api: instance
    )
  }

  public func getCapabilities() throws -> [String?: Any?] {
    let contactsAccess = contactsReadAccess()
    let calendarAccess = calendarReadAccess()

    return [
      "capabilities": [
        capability(
          domain: "contacts",
          read: contactsAccess.readable,
          write: false,
          observe: contactsAccess.readable,
          stream: false,
          reason: contactsAccess.reason
        ),
        capability(domain: "media", read: true, write: true, observe: true, stream: true),
        capability(domain: "files", read: true, write: true, observe: true, stream: true),
        capability(
          domain: "calendar",
          read: calendarAccess.readable,
          write: false,
          observe: calendarAccess.readable,
          stream: false,
          reason: calendarAccess.reason
        ),
        capability(domain: "messages", read: false, write: false, observe: false, stream: false,
                   reason: "simple_query: messages is not supported on macOS"),
        capability(domain: "calls", read: false, write: false, observe: false, stream: false,
                   reason: "simple_query: calls is not supported on macOS"),
        capability(domain: "platformSpecific", read: true, write: false, observe: false, stream: false),
      ],
      "platformExtensions": [
        "macos.contacts": true,
        "macos.calendar": true,
        "macos.photos": true,
      ]
    ]
  }

  public func query(request: [String?: Any?]) throws -> [String?: Any?] {
    let domain = (request["domain"] as? String) ?? "platformSpecific"
    var records: [[String?: Any?]]
    var metadata: [String: Any?] = [:]

    switch domain {
    case "files", "media":
      let rootPath = resolveRootPath(request: request)
      let mediaOnly = domain == "media"
      records = try listRecords(rootPath: rootPath, mediaOnly: mediaOnly)
      metadata = [
        "rootPath": rootPath,
        "mediaOnly": mediaOnly,
      ]
    case "contacts":
      records = try listContactRecords()
    case "calendar":
      records = try listCalendarRecords(request: request)
    default:
      throw notSupported("simple_query: query is not supported for domain \(domain) on macOS host")
    }

    records = applyFilters(records: records, filters: request["filters"] as? [[String?: Any?]] ?? [])
    records = applySort(records: records, sort: request["sort"] as? [[String?: Any?]] ?? [])

    let totalCount = records.count
    let page = request["page"] as? [String?: Any?]
    let offset = page?["offset"] as? Int ?? 0
    let limit = page?["limit"] as? Int
    let paged = applyPaging(records: records, limit: limit, offset: offset)

    let projection = request["projection"] as? [String]
    let projected = applyProjection(records: paged.records, projection: projection)

    return [
      "records": projected,
      "totalCount": totalCount,
      "nextOffset": paged.nextOffset,
      "metadata": metadata,
    ]
  }

  public func mutate(request: [String?: Any?]) throws -> [String?: Any?] {
    let domain = (request["domain"] as? String) ?? "platformSpecific"
    guard domain == "files" || domain == "media" else {
      throw notSupported("simple_query: mutate is not supported for domain \(domain) on macOS host")
    }

    guard let type = request["type"] as? String else {
      throw invalidQuery("simple_query: mutation type is required")
    }

    switch type {
    case "insert":
      guard let values = request["values"] as? [String?: Any?],
            let path = values["path"] as? String,
            !path.isEmpty else {
        throw invalidQuery("simple_query: insert requires values.path")
      }

      let isDirectory = (values["isDirectory"] as? Bool) == true
      if isDirectory {
        try FileManager.default.createDirectory(
          at: URL(fileURLWithPath: path),
          withIntermediateDirectories: true
        )
      } else {
        let fileUrl = URL(fileURLWithPath: path)
        try FileManager.default.createDirectory(
          at: fileUrl.deletingLastPathComponent(),
          withIntermediateDirectories: true
        )
        if let bytes = values["bytes"] as? FlutterStandardTypedData {
          try bytes.data.write(to: fileUrl)
        } else if let bytes = values["bytes"] as? Data {
          try bytes.write(to: fileUrl)
        } else {
          let content = (values["content"] as? String) ?? ""
          try content.data(using: .utf8)?.write(to: fileUrl)
        }
      }

      return [
        "affectedCount": 1,
        "insertedId": path,
      ]

    case "delete":
      let queryResult = try query(request: [
        "domain": domain,
        "filters": request["filters"] as? [[String?: Any?]] ?? [],
        "platformData": request["platformData"] as Any,
      ])
      let rows = queryResult["records"] as? [[String?: Any?]] ?? []
      var deleted = 0
      for row in rows {
        guard let path = row["path"] as? String, !path.isEmpty else { continue }
        do {
          try FileManager.default.removeItem(atPath: path)
          deleted += 1
        } catch {
          // Best-effort delete behavior.
        }
      }
      return ["affectedCount": deleted]

    case "update":
      guard let values = request["values"] as? [String?: Any?] else {
        throw invalidQuery("simple_query: update requires values")
      }

      var targetPaths = Set<String>()
      let explicitPath = (values["path"] as? String) ?? (values["id"] as? String)
      if let explicitPath, !explicitPath.isEmpty {
        targetPaths.insert(explicitPath)
      }

      if targetPaths.isEmpty {
        let queryResult = try query(request: [
          "domain": domain,
          "filters": request["filters"] as? [[String?: Any?]] ?? [],
          "platformData": request["platformData"] as Any,
        ])
        let rows = queryResult["records"] as? [[String?: Any?]] ?? []
        for row in rows {
          if let path = row["path"] as? String, !path.isEmpty {
            targetPaths.insert(path)
          }
        }
      }

      var updated = 0
      for path in targetPaths {
        if try applyLocalUpdate(path: path, values: values) {
          updated += 1
        }
      }
      return ["affectedCount": updated]

    default:
      throw invalidQuery("simple_query: unknown mutation type \(type)")
    }
  }

  public func batch(request: [String?: Any?]) throws -> [String?: Any?] {
    let operations = request["operations"] as? [[String?: Any?]] ?? []
    let platformData = request["platformData"]

    var results: [[String?: Any?]] = []
    for op in operations {
      var merged = op
      if merged["platformData"] == nil {
        merged["platformData"] = platformData
      }
      results.append(try mutate(request: merged))
    }

    return ["results": results]
  }

  public func observeStart(request: [String?: Any?]) throws -> String {
    let domain = (request["domain"] as? String) ?? "platformSpecific"
    guard domain == "files" || domain == "media" || domain == "contacts" || domain == "calendar" else {
      throw notSupported("simple_query: observe is not supported for domain \(domain) on macOS host")
    }

    let observerId = UUID().uuidString
    let intervalMs = (request["pollingIntervalMs"] as? Int) ?? 1000
    let interval = TimeInterval(max(intervalMs, 250)) / 1000.0

    snapshots[observerId] = try currentSnapshot(request: request)

    let timer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in
      guard let self = self else { return }
      let next = (try? self.currentSnapshot(request: request)) ?? [:]
      let previous = self.snapshots[observerId] ?? [:]
      guard next != previous else { return }

      let changedIds = Set(previous.keys).symmetricDifference(Set(next.keys))
      self.snapshots[observerId] = next

      let event: [String?: Any?] = [
        "domain": domain,
        "changeType": "unknown",
        "timestamp": ISO8601DateFormatter().string(from: Date()),
        "ids": Array(changedIds),
        "source": "macos-host",
      ]

      self.flutterApi.onObserveEvent(observerId: observerId, event: event) { _ in }
    }

    observers[observerId] = timer
    return observerId
  }

  public func observeStop(observerId: String) throws {
    observers.removeValue(forKey: observerId)?.invalidate()
    snapshots.removeValue(forKey: observerId)
  }

  public func openBinary(request: [String?: Any?]) throws -> [String?: Any?] {
    let domain = (request["domain"] as? String) ?? "platformSpecific"
    guard domain == "files" || domain == "media" else {
      throw notSupported("simple_query: stream is not supported for domain \(domain) on macOS host")
    }

    let platformData = request["platformData"] as? [String?: Any?]
    let requestedPath = (platformData?["path"] as? String) ?? (request["recordId"] as? String)

    guard let path = requestedPath, FileManager.default.fileExists(atPath: path) else {
      throw unavailable("simple_query: binary resource was not found")
    }

    let attrs = try FileManager.default.attributesOfItem(atPath: path)
    let size = (attrs[.size] as? NSNumber)?.intValue
    let mimeType = mimeType(forPath: path)

    let handleId = UUID().uuidString
    openHandles[handleId] = path

    return [
      "handleId": handleId,
      "localPath": path,
      "mimeType": mimeType,
      "size": size,
      "metadata": [
        "source": "macos-host",
      ],
    ]
  }

  public func closeBinary(handleId: String) throws {
    openHandles.removeValue(forKey: handleId)
  }

  public func callExtension(
    namespace: String,
    method: String,
    args: [String?: Any?]?
  ) throws -> [String?: Any?]? {
    switch namespace {
    case "macos.photos":
      if method == "fetchAssetResources" {
        let limitValue = args?["limit"]
        if let limitValue, !(limitValue is Int) {
          throw invalidQuery("simple_query: macos.photos.fetchAssetResources expects limit as Int")
        }
        let rootPathValue = args?["rootPath"]
        if let rootPathValue, !(rootPathValue is String) {
          throw invalidQuery("simple_query: macos.photos.fetchAssetResources expects rootPath as String")
        }
        let rootPath = (args?["rootPath"] as? String) ?? defaultRootPath()
        let records = try listRecords(rootPath: rootPath, mediaOnly: true)
        let limit = args?["limit"] as? Int
        let resources = records.map { row in
          [
            "id": row["id"] as Any,
            "uriOrPath": row["uriOrPath"] as Any,
            "mimeType": row["mimeType"] as Any,
            "size": row["size"] as Any,
          ]
        }
        return [
          "resources": limit == nil ? resources : Array(resources.prefix(max(0, limit!)))
        ]
      }
      if method == "listMediaTypes" {
        if args != nil, !(args?.isEmpty ?? true) {
          throw invalidQuery("simple_query: macos.photos.listMediaTypes does not accept arguments")
        }
        return [
          "mediaTypes": ["image", "video", "audio"],
        ]
      }
    case "macos.contacts":
      if method == "listSources" {
        if args != nil, !(args?.isEmpty ?? true) {
          throw invalidQuery("simple_query: macos.contacts.listSources does not accept arguments")
        }
        return [
          "sources": try listContactContainers(),
          "authorizationStatus": contactsAuthorizationStatusName(),
        ]
      }
      if method == "listGroups" {
        if args != nil, !(args?.isEmpty ?? true) {
          throw invalidQuery("simple_query: macos.contacts.listGroups does not accept arguments")
        }
        return [
          "groups": try listContactGroups(),
          "authorizationStatus": contactsAuthorizationStatusName(),
        ]
      }
    case "macos.calendar":
      if method == "listCalendars" {
        if args != nil, !(args?.isEmpty ?? true) {
          throw invalidQuery("simple_query: macos.calendar.listCalendars does not accept arguments")
        }
        return [
          "calendars": try listCalendars(),
          "authorizationStatus": calendarAuthorizationStatusName(),
        ]
      }
      if method == "getDefaultTimeZone" {
        if args != nil, !(args?.isEmpty ?? true) {
          throw invalidQuery("simple_query: macos.calendar.getDefaultTimeZone does not accept arguments")
        }
        return [
          "timeZone": TimeZone.current.identifier,
        ]
      }
    default:
      break
    }

    throw notSupported("simple_query: \(namespace).\(method) is not supported on macOS host")
  }

  private func listContactRecords() throws -> [[String?: Any?]] {
    let access = contactsReadAccess()
    guard access.readable else {
      throw permissionDenied(access.reason ?? "simple_query: contacts permission is not granted")
    }

    let contactStore = CNContactStore()
    let keys: [CNKeyDescriptor] = [
      CNContactIdentifierKey as CNKeyDescriptor,
      CNContactGivenNameKey as CNKeyDescriptor,
      CNContactFamilyNameKey as CNKeyDescriptor,
      CNContactMiddleNameKey as CNKeyDescriptor,
      CNContactOrganizationNameKey as CNKeyDescriptor,
      CNContactPhoneNumbersKey as CNKeyDescriptor,
      CNContactEmailAddressesKey as CNKeyDescriptor,
    ]
    let request = CNContactFetchRequest(keysToFetch: keys)
    var rows: [[String?: Any?]] = []

    try contactStore.enumerateContacts(with: request) { contact, _ in
      let phones = contact.phoneNumbers.map { $0.value.stringValue }
      let emails = contact.emailAddresses.map { String($0.value) }
      let displayName = CNContactFormatter.string(from: contact, style: .fullName)
        ?? "\(contact.givenName) \(contact.familyName)".trimmingCharacters(in: .whitespaces)
      let organization = contact.organizationName.isEmpty ? nil : contact.organizationName

      rows.append([
        "id": contact.identifier,
        "displayName": displayName,
        "phones": phones,
        "emails": emails,
        "organization": organization,
        "updatedAt": nil,
      ])
    }

    return rows
  }

  private func applyLocalUpdate(path: String, values: [String?: Any?]) throws -> Bool {
    var isDirectory: ObjCBool = false
    let exists = FileManager.default.fileExists(atPath: path, isDirectory: &isDirectory)
    guard exists else { return false }

    var effectivePath = path
    var changed = false

    let newPath = values["newPath"] as? String
    if let newPath, !newPath.isEmpty, newPath != path {
      let targetUrl = URL(fileURLWithPath: newPath)
      try FileManager.default.createDirectory(
        at: targetUrl.deletingLastPathComponent(),
        withIntermediateDirectories: true
      )
      try FileManager.default.moveItem(atPath: path, toPath: newPath)
      effectivePath = newPath
      changed = true
    }

    if isDirectory.boolValue {
      return changed
    }

    let fileUrl = URL(fileURLWithPath: effectivePath)
    if let bytes = values["bytes"] as? FlutterStandardTypedData {
      try bytes.data.write(to: fileUrl)
      return true
    }
    if let bytes = values["bytes"] as? Data {
      try bytes.write(to: fileUrl)
      return true
    }
    if let content = values["content"] as? String {
      try content.data(using: .utf8)?.write(to: fileUrl)
      return true
    }
    if values.keys.contains(where: { $0 == "content" }) {
      try Data().write(to: fileUrl)
      return true
    }
    return changed
  }

  private func listCalendarRecords(request: [String?: Any?]) throws -> [[String?: Any?]] {
    let access = calendarReadAccess()
    guard access.readable else {
      throw permissionDenied(access.reason ?? "simple_query: calendar permission is not granted")
    }

    let eventStore = EKEventStore()
    let platformData = request["platformData"] as? [String?: Any?]
    let start = parseIso8601(platformData?["startAt"] as? String) ?? Date().addingTimeInterval(-31_536_000)
    let end = parseIso8601(platformData?["endAt"] as? String) ?? Date().addingTimeInterval(31_536_000)
    let predicate = eventStore.predicateForEvents(withStart: start, end: end, calendars: nil)

    return eventStore.events(matching: predicate).map { event in
      [
        "id": event.eventIdentifier ?? UUID().uuidString,
        "title": event.title ?? "",
        "startAt": event.startDate.iso8601,
        "endAt": event.endDate.iso8601,
        "isAllDay": event.isAllDay,
        "calendarId": event.calendar.calendarIdentifier,
        "updatedAt": event.lastModifiedDate?.iso8601,
      ]
    }
  }

  private func listContactContainers() throws -> [[String: Any?]] {
    let access = contactsReadAccess()
    guard access.readable else {
      throw permissionDenied(access.reason ?? "simple_query: contacts permission is not granted")
    }

    let contactStore = CNContactStore()
    return try contactStore.containers(matching: nil).map { container in
      [
        "id": container.identifier,
        "name": container.name,
        "type": container.type.rawValue,
      ]
    }
  }

  private func listContactGroups() throws -> [[String: Any?]] {
    let access = contactsReadAccess()
    guard access.readable else {
      throw permissionDenied(access.reason ?? "simple_query: contacts permission is not granted")
    }

    let contactStore = CNContactStore()
    return try contactStore.groups(matching: nil).map { group in
      [
        "id": group.identifier,
        "name": group.name,
      ]
    }
  }

  private func listCalendars() throws -> [[String: Any?]] {
    let access = calendarReadAccess()
    guard access.readable else {
      throw permissionDenied(access.reason ?? "simple_query: calendar permission is not granted")
    }

    let eventStore = EKEventStore()
    return eventStore.calendars(for: .event).map { calendar in
      [
        "id": calendar.calendarIdentifier,
        "title": calendar.title,
        "type": calendar.type.rawValue,
      ]
    }
  }

  private func contactsReadAccess() -> (readable: Bool, reason: String?) {
    switch CNContactStore.authorizationStatus(for: .contacts) {
    case .authorized:
      return (true, nil)
    case .notDetermined:
      return (false, "simple_query: contacts permission has not been granted on macOS")
    case .restricted, .denied:
      return (false, "simple_query: contacts access is denied on macOS")
    @unknown default:
      return (false, "simple_query: contacts access is unavailable on macOS")
    }
  }

  private func contactsAuthorizationStatusName() -> String {
    switch CNContactStore.authorizationStatus(for: .contacts) {
    case .authorized:
      return "authorized"
    case .notDetermined:
      return "notDetermined"
    case .restricted:
      return "restricted"
    case .denied:
      return "denied"
    @unknown default:
      return "unknown"
    }
  }

  private func calendarReadAccess() -> (readable: Bool, reason: String?) {
    if #available(macOS 14.0, *) {
      switch EKEventStore.authorizationStatus(for: .event) {
      case .fullAccess:
        return (true, nil)
      case .writeOnly:
        return (false, "simple_query: calendar read access is unavailable (write-only) on macOS")
      case .notDetermined:
        return (false, "simple_query: calendar permission has not been granted on macOS")
      case .restricted, .denied:
        return (false, "simple_query: calendar access is denied on macOS")
      @unknown default:
        return (false, "simple_query: calendar access is unavailable on macOS")
      }
    }

    switch EKEventStore.authorizationStatus(for: .event) {
    case .authorized:
      return (true, nil)
    case .notDetermined:
      return (false, "simple_query: calendar permission has not been granted on macOS")
    case .restricted, .denied:
      return (false, "simple_query: calendar access is denied on macOS")
    @unknown default:
      return (false, "simple_query: calendar access is unavailable on macOS")
    }
  }

  private func calendarAuthorizationStatusName() -> String {
    if #available(macOS 14.0, *) {
      switch EKEventStore.authorizationStatus(for: .event) {
      case .fullAccess:
        return "fullAccess"
      case .writeOnly:
        return "writeOnly"
      case .notDetermined:
        return "notDetermined"
      case .restricted:
        return "restricted"
      case .denied:
        return "denied"
      @unknown default:
        return "unknown"
      }
    }

    switch EKEventStore.authorizationStatus(for: .event) {
    case .authorized:
      return "authorized"
    case .notDetermined:
      return "notDetermined"
    case .restricted:
      return "restricted"
    case .denied:
      return "denied"
    @unknown default:
      return "unknown"
    }
  }

  private func parseIso8601(_ raw: String?) -> Date? {
    guard let raw, !raw.isEmpty else { return nil }
    return ISO8601DateFormatter().date(from: raw)
  }

  private func resolveRootPath(request: [String?: Any?]) -> String {
    let platformData = request["platformData"] as? [String?: Any?]
    return (platformData?["rootPath"] as? String) ?? defaultRootPath()
  }

  private func defaultRootPath() -> String {
    FileManager.default.homeDirectoryForCurrentUser.path
  }

  private func listRecords(rootPath: String, mediaOnly: Bool) throws -> [[String?: Any?]] {
    let manager = FileManager.default
    guard let enumerator = manager.enumerator(atPath: rootPath) else {
      return []
    }

    var rows: [[String?: Any?]] = []
    while let relative = enumerator.nextObject() as? String {
      let absolutePath = URL(fileURLWithPath: rootPath).appendingPathComponent(relative).path
      let attrs = try? manager.attributesOfItem(atPath: absolutePath)
      let isDirectory = ((attrs?[.type] as? FileAttributeType) == .typeDirectory)
      let mime = mimeType(forPath: absolutePath)

      if mediaOnly {
        if isDirectory || !isMediaMime(mime) {
          continue
        }

        rows.append([
          "id": absolutePath,
          "uriOrPath": absolutePath,
          "mediaType": mediaType(forMime: mime),
          "mimeType": mime,
          "size": (attrs?[.size] as? NSNumber)?.intValue,
          "createdAt": (attrs?[.creationDate] as? Date)?.iso8601,
          "modifiedAt": (attrs?[.modificationDate] as? Date)?.iso8601,
        ])
      } else {
        rows.append([
          "id": absolutePath,
          "path": absolutePath,
          "name": URL(fileURLWithPath: absolutePath).lastPathComponent,
          "isDirectory": isDirectory,
          "size": (attrs?[.size] as? NSNumber)?.intValue,
          "modifiedAt": (attrs?[.modificationDate] as? Date)?.iso8601,
          "mimeType": mime,
          "type": isDirectory ? "directory" : "file",
          "extension": URL(fileURLWithPath: absolutePath).pathExtension.lowercased(),
          "modifiedEpochMs": Int(((attrs?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0) * 1000),
        ])
      }
    }

    return rows
  }

  private func applyFilters(records: [[String?: Any?]], filters: [[String?: Any?]]) -> [[String?: Any?]] {
    guard !filters.isEmpty else { return records }

    return records.filter { record in
      for filter in filters {
        guard let field = filter["field"] as? String,
              let op = filter["operator"] as? String else {
          continue
        }

        let actual = record[field] ?? nil
        let expected = filter["value"] ?? nil

        switch op {
        case "equals":
          if String(describing: actual ?? "") != String(describing: expected ?? "") { return false }
        case "contains":
          let left = String(describing: actual ?? "").lowercased()
          let right = String(describing: expected ?? "").lowercased()
          if !left.contains(right) { return false }
        case "inList":
          if let values = expected as? [Any] {
            let target = String(describing: actual ?? "")
            let allowed = values.map { String(describing: $0) }
            if !allowed.contains(target) { return false }
          }
        default:
          break
        }
      }
      return true
    }
  }

  private func applySort(records: [[String?: Any?]], sort: [[String?: Any?]]) -> [[String?: Any?]] {
    guard let spec = sort.first,
          let field = spec["field"] as? String else {
      return records
    }

    let ascending = (spec["direction"] as? String) != "descending"

    return records.sorted { lhs, rhs in
      let left = String(describing: lhs[field] ?? "")
      let right = String(describing: rhs[field] ?? "")
      return ascending ? (left < right) : (left > right)
    }
  }

  private func applyPaging(records: [[String?: Any?]], limit: Int?, offset: Int) -> (records: [[String?: Any?]], nextOffset: Int?) {
    let start = max(0, min(offset, records.count))
    let end: Int
    if let limit {
      end = min(records.count, start + max(0, limit))
    } else {
      end = records.count
    }

    let slice = Array(records[start..<end])
    let nextOffset = end < records.count ? end : nil
    return (slice, nextOffset)
  }

  private func applyProjection(records: [[String?: Any?]], projection: [String]?) -> [[String?: Any?]] {
    guard let projection, !projection.isEmpty else { return records }

    return records.map { row in
      var projected: [String?: Any?] = [:]
      for key in projection {
        projected[key] = row[key] ?? nil
      }
      return projected
    }
  }

  private func currentSnapshot(request: [String?: Any?]) throws -> [String: Int64] {
    var snapshot: [String: Int64] = [:]
    let rows = (try query(request: request)["records"] as? [[String?: Any?]]) ?? []
    for row in rows {
      guard let id = row["id"] as? String else { continue }
      let modifiedRaw = row["modifiedEpochMs"] as? Int
      let updatedAt = row["updatedAt"] as? String
      let startAt = row["startAt"] as? String
      let modified = modifiedRaw
        ?? parseIso8601(updatedAt)?.epochMs
        ?? parseIso8601(startAt)?.epochMs
        ?? 0
      snapshot[id] = Int64(modified)
    }
    return snapshot
  }

  private func capability(
    domain: String,
    read: Bool,
    write: Bool,
    observe: Bool,
    stream: Bool,
    reason: String? = nil
  ) -> [String: Any?] {
    return [
      "domain": domain,
      "canRead": read,
      "canWrite": write,
      "canObserve": observe,
      "canStream": stream,
      "reason": reason,
    ]
  }

  private func isMediaMime(_ mime: String) -> Bool {
    mime.hasPrefix("image/") || mime.hasPrefix("video/") || mime.hasPrefix("audio/")
  }

  private func mediaType(forMime mime: String) -> String {
    if mime.hasPrefix("image/") { return "image" }
    if mime.hasPrefix("video/") { return "video" }
    if mime.hasPrefix("audio/") { return "audio" }
    return "other"
  }

  private func mimeType(forPath path: String) -> String {
    let ext = URL(fileURLWithPath: path).pathExtension.lowercased()
    switch ext {
    case "jpg", "jpeg": return "image/jpeg"
    case "png": return "image/png"
    case "gif": return "image/gif"
    case "heic": return "image/heic"
    case "mp4": return "video/mp4"
    case "mov": return "video/quicktime"
    case "mp3": return "audio/mpeg"
    case "wav": return "audio/wav"
    case "aac": return "audio/aac"
    case "flac": return "audio/flac"
    case "txt": return "text/plain"
    case "json": return "application/json"
    default: return "application/octet-stream"
    }
  }

  private func notSupported(_ message: String) -> NativeQueryPigeonError {
    NativeQueryPigeonError(code: "not-supported", message: message, details: nil)
  }

  private func invalidQuery(_ message: String) -> NativeQueryPigeonError {
    NativeQueryPigeonError(code: "invalid-query", message: message, details: nil)
  }

  private func unavailable(_ message: String) -> NativeQueryPigeonError {
    NativeQueryPigeonError(code: "unavailable", message: message, details: nil)
  }

  private func permissionDenied(_ message: String) -> NativeQueryPigeonError {
    NativeQueryPigeonError(code: "permission-denied", message: message, details: nil)
  }
}

private extension Date {
  var iso8601: String {
    ISO8601DateFormatter().string(from: self)
  }

  var epochMs: Int {
    Int(timeIntervalSince1970 * 1000)
  }
}
