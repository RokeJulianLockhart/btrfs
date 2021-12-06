#include "test.h"

using namespace std;

static const uint8_t sid_everyone[] = { 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 }; // S-1-1-0

void set_dacl(HANDLE h, ACCESS_MASK access) {
    NTSTATUS Status;
    SECURITY_DESCRIPTOR sd;
    array<uint8_t, sizeof(ACL) + offsetof(ACCESS_ALLOWED_ACE, SidStart) + sizeof(sid_everyone)> aclbuf;

    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION))
        throw formatted_error("InitializeSecurityDescriptor failed (error {})", GetLastError());

    auto& acl = *(ACL*)aclbuf.data();

    if (!InitializeAcl(&acl, aclbuf.size(), ACL_REVISION))
        throw formatted_error("InitializeAcl failed (error {})", GetLastError());

    if (access != 0) {
        acl.AceCount = 1;

        auto& ace = *(ACCESS_ALLOWED_ACE*)((uint8_t*)aclbuf.data() + sizeof(ACL));

        ace.Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
        ace.Header.AceFlags = 0;
        ace.Header.AceSize = offsetof(ACCESS_ALLOWED_ACE, SidStart) + sizeof(sid_everyone);
        ace.Mask = access;
        memcpy(&ace.SidStart, sid_everyone, sizeof(sid_everyone));
    }

    if (!SetSecurityDescriptorDacl(&sd, true, &acl, false))
        throw formatted_error("SetSecurityDescriptorDacl failed (error {})", GetLastError());

    Status = NtSetSecurityObject(h, DACL_SECURITY_INFORMATION, &sd);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);
}

static vector<varbuf<ACE_HEADER>> get_dacl(HANDLE h) {
    NTSTATUS Status;
    ULONG needed = 0;
    vector<uint8_t> buf;
    vector<varbuf<ACE_HEADER>> ret;

    Status = NtQuerySecurityObject(h, DACL_SECURITY_INFORMATION, nullptr, 0, &needed);

    if (Status != STATUS_BUFFER_TOO_SMALL)
        throw ntstatus_error(Status);

    buf.resize(needed);

    Status = NtQuerySecurityObject(h, DACL_SECURITY_INFORMATION, buf.data(), buf.size(), &needed);

    if (Status != STATUS_SUCCESS)
        throw ntstatus_error(Status);

    if (buf.size() < sizeof(SECURITY_DESCRIPTOR_RELATIVE))
        throw formatted_error("SD was {} bytes, expected at least {}", buf.size(), sizeof(SECURITY_DESCRIPTOR_RELATIVE));

    auto& sd = *(SECURITY_DESCRIPTOR_RELATIVE*)buf.data();

    if (sd.Revision != 1)
        throw formatted_error("SD revision was {}, expected 1", sd.Revision);

    if (sd.Dacl == 0)
        return {};

    if (sd.Dacl + sizeof(ACL) > buf.size())
        throw runtime_error("DACL extended beyond end of SD");

    auto& acl = *(ACL*)(buf.data() + sd.Dacl);

    if (acl.AclRevision != ACL_REVISION)
        throw formatted_error("ACL revision was {}, expected {}", acl.AclRevision, ACL_REVISION);

    if (acl.AclSize < sizeof(ACL))
        throw formatted_error("ACL size was {}, expected at least {}", acl.AclSize, sizeof(ACL));

    ret.resize(acl.AceCount);

    auto aclsp = span<const uint8_t>((uint8_t*)&acl + sizeof(ACL), acl.AclSize - sizeof(ACL));

    for (unsigned int i = 0; i < acl.AceCount; i++) {
        auto& ace = *(ACE_HEADER*)aclsp.data();

        if (aclsp.size() < sizeof(ACE_HEADER))
            throw formatted_error("Not enough bytes left for ACE ({} < {})", aclsp.size(), sizeof(ACE_HEADER));

        if (aclsp.size() < ace.AceSize)
            throw formatted_error("ACE overflowed end of SD ({} < {})", aclsp.size(), ace.AceSize);

        auto& b = ret[i].buf;

        b.resize(ace.AceSize);
        memcpy(b.data(), &ace, ace.AceSize);

        aclsp = aclsp.subspan(ace.AceSize);
    }

    return ret;
}

static string sid_to_string(span<const uint8_t> sid) {
    string s;
    auto& ss = *(SID*)sid.data();

    if (sid.size() < offsetof(SID, SubAuthority) || ss.Revision != 1 || sid.size() < offsetof(SID, SubAuthority) + (ss.SubAuthorityCount * sizeof(ULONG))) {
        for (auto b : sid) {
            if (!s.empty())
                s += " ";

            s += fmt::format("{:02x}", b);
        }

        return "Malformed SID (" + s + ")";
    }

    uint64_t auth;

    auth = (uint64_t)sid[2] << 40;
    auth |= (uint64_t)sid[3] << 32;
    auth |= (uint64_t)sid[4] << 24;
    auth |= (uint64_t)sid[5] << 16;
    auth |= (uint64_t)sid[6] << 8;
    auth |= sid[7];

    s = fmt::format("S-1-{}", auth);

    auto sub = span<const ULONG>(ss.SubAuthority, ss.SubAuthorityCount);

    for (auto n : sub) {
        s += fmt::format("-{}", n);
    }

    return s;
}

static bool compare_sid(span<const uint8_t> sid1, span<const uint8_t> sid2) {
    if (sid1.size() < offsetof(SID, SubAuthority) || sid2.size() < offsetof(SID, SubAuthority))
        throw runtime_error("Malformed SID");

    auto& ss1 = *(SID*)sid1.data();
    auto& ss2 = *(SID*)sid2.data();

    if (ss1.Revision != 1 || ss2.Revision != 1)
        throw runtime_error("Unknown SID revision");

    auto len1 = offsetof(SID, SubAuthority) + (ss1.SubAuthorityCount * sizeof(ULONG));
    auto len2 = offsetof(SID, SubAuthority) + (ss2.SubAuthorityCount * sizeof(ULONG));

    if (len1 != len2)
        return false;

    return !memcmp(sid1.data(), sid2.data(), len1);
}

void test_security(const u16string& dir) {
    unique_handle h;

    test("Create file", [&]() {
        h = create_file(dir + u"\\sec1", GENERIC_READ, 0, 0, FILE_CREATE, 0, FILE_CREATED);
    });

    if (h) {
        test("Query FileAccessInformation", [&]() {
            auto fai = query_information<FILE_ACCESS_INFORMATION>(h.get());

            ACCESS_MASK exp = SYNCHRONIZE | READ_CONTROL | FILE_READ_ATTRIBUTES |
                              FILE_READ_EA | FILE_READ_DATA;

            if (fai.AccessFlags != exp)
                throw formatted_error("AccessFlags was {:x}, expected {:x}", fai.AccessFlags, exp);
        });

        h.reset();
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\sec1", GENERIC_WRITE, 0, 0, FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        test("Query FileAccessInformation", [&]() {
            auto fai = query_information<FILE_ACCESS_INFORMATION>(h.get());

            ACCESS_MASK exp = SYNCHRONIZE | READ_CONTROL | FILE_WRITE_ATTRIBUTES |
                              FILE_WRITE_EA | FILE_APPEND_DATA | FILE_WRITE_DATA;

            if (fai.AccessFlags != exp)
                throw formatted_error("AccessFlags was {:x}, expected {:x}", fai.AccessFlags, exp);
        });

        h.reset();
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\sec1", GENERIC_EXECUTE, 0, 0, FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        test("Query FileAccessInformation", [&]() {
            auto fai = query_information<FILE_ACCESS_INFORMATION>(h.get());

            ACCESS_MASK exp = SYNCHRONIZE | READ_CONTROL | FILE_READ_ATTRIBUTES |
                              FILE_EXECUTE;

            if (fai.AccessFlags != exp)
                throw formatted_error("AccessFlags was {:x}, expected {:x}", fai.AccessFlags, exp);
        });

        h.reset();
    }

    test("Open file", [&]() {
        h = create_file(dir + u"\\sec1", READ_CONTROL | WRITE_DAC, 0, 0, FILE_OPEN, 0, FILE_OPENED);
    });

    if (h) {
        ACCESS_MASK access = SYNCHRONIZE | WRITE_OWNER | WRITE_DAC | READ_CONTROL | DELETE |
                             FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | FILE_DELETE_CHILD |
                             FILE_EXECUTE | FILE_WRITE_EA | FILE_READ_EA | FILE_APPEND_DATA |
                             FILE_WRITE_DATA | FILE_READ_DATA;

        test("Set DACL to maximum for Everyone", [&]() {
            set_dacl(h.get(), access);
        });

        test("Query DACL", [&]() {
            auto items = get_dacl(h.get());

            if (items.size() != 1)
                throw formatted_error("{} items returned, expected 1", items.size());

            auto& ace = *static_cast<ACE_HEADER*>(items.front());

            if (ace.AceType != ACCESS_ALLOWED_ACE_TYPE)
                throw formatted_error("ACE type was {}, expected ACCESS_ALLOWED_ACE_TYPE", ace.AceType);

            if (ace.AceFlags != 0)
                throw formatted_error("AceFlags was {:x}, expected 0", ace.AceFlags);

            auto& aaa = *reinterpret_cast<ACCESS_ALLOWED_ACE*>(&ace);

            if (aaa.Mask != access)
                throw formatted_error("Mask was {:x}, expected {:x}", aaa.Mask, access);

            auto sid = span<const uint8_t>((uint8_t*)&aaa.SidStart, items.front().buf.size() - offsetof(ACCESS_ALLOWED_ACE, SidStart));

            if (!compare_sid(sid, sid_everyone))
                throw formatted_error("SID was {}, expected {}", sid_to_string(sid), sid_to_string(sid_everyone));
        });

        h.reset();
    }

    // FIXME - querying SD
    // FIXME - setting SD (owner, group, SACL)
    // FIXME - creating file with SD
    // FIXME - inheriting SD
    // FIXME - open files asking for too many permissions
    // FIXME - MAXIMUM_ALLOWED
    // FIXME - permissions needed for querying and setting SD
    // FIXME - backup and restore privileges
    // FIXME - traverse checking
    // FIXME - make sure empty DACL means no permissions?
    // FIXME - make sure mandatory access controls etc. obeyed (inc. when traverse-checking)
}
