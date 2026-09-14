#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $# != 2 ]]; then
    echo 'usage: create_usage_pki.sh OUTPUT_DIRECTORY ADAPTER_IPV4' >&2
    exit 2
fi
output=$1
adapter_ip=$2
if ! [[ "$adapter_ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]]; then
    echo 'ADAPTER_IPV4 must be an IPv4 address' >&2
    exit 2
fi
IFS=. read -r ip_a ip_b ip_c ip_d <<< "$adapter_ip"
for part in "$ip_a" "$ip_b" "$ip_c" "$ip_d"; do
    if (( 10#$part > 255 )); then exit 2; fi
done
if [[ -e "$output" ]]; then
    echo 'Output directory must not exist; refusing to overwrite credentials.' >&2
    exit 2
fi
mkdir -p "$output"
chmod 700 "$output"
openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 3650 \
    -subj '/CN=SubConverter Usage Private CA' \
    -addext 'basicConstraints=critical,CA:TRUE' \
    -addext 'keyUsage=critical,keyCertSign,cRLSign' \
    -keyout "$output/ca.key" -out "$output/ca.crt" 2>/dev/null
openssl req -new -newkey rsa:3072 -nodes -sha256 \
    -subj '/CN=sui-usage-adapter' \
    -addext "subjectAltName=IP:$adapter_ip" \
    -addext 'basicConstraints=critical,CA:FALSE' \
    -addext 'keyUsage=critical,digitalSignature,keyEncipherment' \
    -addext 'extendedKeyUsage=serverAuth' \
    -keyout "$output/server.key" -out "$output/server.csr" 2>/dev/null
openssl x509 -req -in "$output/server.csr" -CA "$output/ca.crt" \
    -CAkey "$output/ca.key" -CAcreateserial -days 365 -sha256 \
    -copy_extensions copy -out "$output/server.crt" 2>/dev/null
openssl req -new -newkey rsa:3072 -nodes -sha256 \
    -subj '/CN=subconverter-usage-client' \
    -addext 'subjectAltName=URI:spiffe://subconverter/usage-client' \
    -addext 'basicConstraints=critical,CA:FALSE' \
    -addext 'keyUsage=critical,digitalSignature,keyEncipherment' \
    -addext 'extendedKeyUsage=clientAuth' \
    -keyout "$output/client.key" -out "$output/client.csr" 2>/dev/null
openssl x509 -req -in "$output/client.csr" -CA "$output/ca.crt" \
    -CAkey "$output/ca.key" -CAserial "$output/ca.srl" -days 365 -sha256 \
    -copy_extensions copy -out "$output/client.crt" 2>/dev/null
openssl verify -CAfile "$output/ca.crt" -purpose sslserver "$output/server.crt"
openssl verify -CAfile "$output/ca.crt" -purpose sslclient "$output/client.crt"
echo 'Certificates created. Keep ca.key offline; deploy only the required leaf key and CA certificate to each host.'
