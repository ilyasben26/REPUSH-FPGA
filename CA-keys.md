
```sh
python3 - <<'PY'
from nacl.signing import SigningKey

sk = SigningKey.generate()
pk = sk.verify_key

print("seed:", sk.encode().hex())
print("pub :", pk.encode().hex())
PY

# seed: 219bb7589201faf94e62ef9cecd4ee01506e41d604e9c2d8c79267304959efee
# pub : 860683f2ac76cafb0140051b0b36125af200461656041f0082b2a16e5c32358a

python3 - <<'PY'
from nacl.signing import SigningKey

seed_hex = "219bb7589201faf94e62ef9cecd4ee01506e41d604e9c2d8c79267304959efee"
msg = b"hello there"

sk = SigningKey(bytes.fromhex(seed_hex))
sig = sk.sign(msg).signature
print(sig.hex())
PY
```