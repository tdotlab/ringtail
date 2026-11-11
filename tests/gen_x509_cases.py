#!/usr/bin/env python3
"""Writes x509_cases.h: certificate paths, and whether each must be
accepted, from C2SP's x509-limbo (github.com/C2SP/x509-limbo, Apache 2.0).
Checked in, so test_x509 needs only a compiler; rerun this only to change
the cases.

    python3 tests/gen_x509_cases.py LIMBO_JSON

Every server-side case is taken but those about things not done here
(revocation lists, ML-DSA keys), plus a sample of the BetterTLS name
constraint cases (thousands, much alike). Where limbo's answer and ours
differ on purpose, DIFFER says why, and the test checks for our answer."""
import base64, datetime, ipaddress, json, sys, warnings
from cryptography import x509

warnings.simplefilter("ignore")

# Cases whose expected answer we deliberately do not give.
DIFFER = {
    # The CA/Browser Forum's profile asks CAs for these identifiers;
    # browsers do not refuse certificates without them, and nor do we.
    "rfc5280::aki::leaf-missing-aki": "AKI not required",
    "rfc5280::aki::intermediate-missing-aki": "AKI not required",
    "rfc5280::aki::cross-signed-root-missing-aki": "AKI not required",
    "rfc5280::ski::root-missing-ski": "SKI not required",
    "rfc5280::ski::intermediate-missing-ski": "SKI not required",
    "webpki::aki::root-with-aki-missing-keyidentifier": "a root's AKI is not read",
    "webpki::aki::root-with-aki-authoritycertissuer": "a root's AKI is not read",
    "webpki::aki::root-with-aki-authoritycertserialnumber": "a root's AKI is not read",
    "webpki::aki::root-with-aki-all-fields": "a root's AKI is not read",
    "webpki::aki::root-with-aki-ski-mismatch": "a root's AKI is not read",
    # Certificate policies are not processed.
    "rfc5280::pc::ica-noncritical-pc": "policies not processed",
    # Extended key usage: a leaf without one is for any use (RFC 5280),
    # and whether it is critical changes nothing; a root's is not read
    # (four of Mozilla's roots have one).
    "webpki::eku::ee-without-eku": "a leaf without EKU is accepted (RFC 5280)",
    "webpki::eku::ee-critical-eku": "a critical EKU is accepted",
    "webpki::eku::root-has-eku": "a root's EKU is not read",
    # No public suffix list here ("*.com" is refused all the same: a
    # wildcard needs two labels after it).
    "webpki::san::public-suffix-multi-label-wildcard-san": "no public suffix list",
    "webpki::san::public-suffix-private-namespace-wildcard-san": "no public suffix list",
    # The common name is never read, so whether it repeats a subjectAltName
    # (CABF 7.1.4.3) is not checked either.
    "webpki::cn::ipv4-hex-mismatch": "the common name is not read",
    "webpki::cn::ipv4-leading-zeros-mismatch": "the common name is not read",
    "webpki::cn::ipv6-uppercase-mismatch": "the common name is not read",
    "webpki::cn::ipv6-uncompressed-mismatch": "the common name is not read",
    "webpki::cn::ipv6-non-rfc5952-mismatch": "the common name is not read",
    "webpki::cn::punycode-not-in-san": "the common name is not read",
    "webpki::cn::utf8-vs-punycode-mismatch": "the common name is not read",
    "webpki::cn::not-in-san": "the common name is not read",
    "webpki::cn::case-mismatch": "the common name is not read",
    # Where RFC 5280 and the Web PKI disagree, the Web PKI's way.
    "rfc5280::ca-as-leaf": "a CA as the leaf is refused (CABF 7.1.2.7)",
    "pathlen::validation-ignores-pathlen-in-leaf": "a CA as the leaf is refused (CABF 7.1.2.7)",
    "rfc5280::nc::permitted-dns-match-noncritical": "non-critical name constraints are allowed (CABF)",
}

SKIP_FEATURES = {"has-crl", "has-mldsa"}
BETTERTLS_EVERY = 25


def der(pem):
    body = "".join(l for l in pem.strip().splitlines() if not l.startswith("-----"))
    return base64.b64decode(body)


def middle(pem):
    try:
        c = x509.load_pem_x509_certificate(pem.encode())
        return c.not_valid_before_utc + (c.not_valid_after_utc - c.not_valid_before_utc) / 2
    except ValueError:
        return datetime.datetime(2025, 1, 1, tzinfo=datetime.timezone.utc)


def main():
    cases = json.load(open(sys.argv[1]))["testcases"]
    certs, index = [], {}

    def cert(pem):
        d = der(pem)
        if d not in index:
            index[d] = len(certs)
            certs.append(d)
        return index[d]

    out_cases, seen_better, skipped = [], 0, 0
    for t in cases:
        if t["validation_kind"] != "SERVER" or SKIP_FEATURES & set(t.get("features") or ()) or not t["expected_peer_name"]:
            skipped += 1
            continue
        if t["id"].startswith("bettertls::"):
            seen_better += 1
            if seen_better % BETTERTLS_EVERY:
                continue
        assert not t["signature_algorithms"] and not t["crls"]
        assert set(t["extended_key_usage"]) <= {"serverAuth"}
        ku = 0
        for k in t["key_usage"]:
            ku |= {"digitalSignature": 1}[k]
        if t["validation_time"]:
            when = datetime.datetime.fromisoformat(t["validation_time"]).astimezone(datetime.timezone.utc)
        else:  # "now": the middle of the leaf's life, so the case does not age
            when = middle(t["peer_certificate"])
        name = t["expected_peer_name"]
        if name["kind"] == "DNS":
            dns, ip = name["value"], ""
        else:
            assert name["kind"] == "IP"
            dns, ip = "", ipaddress.ip_address(name["value"]).packed.hex()
        ok = t["expected_result"] == "SUCCESS"
        if t["id"] in DIFFER:
            ok = not ok
        roots = " ".join(str(cert(p)) for p in t["trusted_certs"])
        inter = " ".join(str(cert(p)) for p in t["untrusted_intermediates"])
        out_cases.append((t["id"], ok, t["id"] in DIFFER, cert(t["peer_certificate"]), roots, inter,
                          when.strftime("%Y%m%d%H%M%S"), dns, ip,
                          6 if t["max_chain_depth"] is None else t["max_chain_depth"], ku))
    missing = set(DIFFER) - {c[0] for c in out_cases}
    assert not missing, missing

    out = ["// Generated by gen_x509_cases.py. Do not edit.",
           "// From C2SP's x509-limbo (github.com/C2SP/x509-limbo, Apache License 2.0).",
           "// Certificates are DER in base64; a case's roots and intermediates are",
           "// lists of their indices.",
           "static const char* const x509_certs[] = {"]
    for d in certs:
        out.append('    "%s",' % base64.b64encode(d).decode())
    out.append("};")
    out.append("typedef struct { const char* id; int ok, differs, leaf; const char *roots, *inter, *when, *dns, *ip;"
               " unsigned max_depth, leaf_ku; } x509_case_t;")
    out.append("static const x509_case_t x509_cases[] = {")
    for c in out_cases:
        out.append('    {"%s", %d, %d, %d, "%s", "%s", "%s", "%s", "%s", %du, %du},' % c)
    out.append("};")
    open(__file__.replace("gen_x509_cases.py", "x509_cases.h"), "w").write("\n".join(out) + "\n")
    print("%d cases (%d differ on purpose, %d of %d BetterTLS), %d certificates, %d skipped" % (
        len(out_cases), len(DIFFER), seen_better // BETTERTLS_EVERY, seen_better, len(certs), skipped))


main()
