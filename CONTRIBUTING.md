# Contributing to PocketFox

Thanks for your interest in improving PocketFox - the native Cocoa browser for PowerPC Macs!

## Quick Start

1. Fork this repository
2. Make your changes
3. Open a Pull Request

## Project Scope

PocketFox is a lightweight browser for Mac OS X Tiger (10.4) and Leopard (10.5) on PowerPC G4/G5.

**Goals:**
- Native Cocoa UI
- Modern TLS 1.2 via mbedTLS
- PowerPC optimization
- Minimal dependencies

## Ways to Contribute

### Code
- UI improvements
- Performance optimizations
- Bug fixes
- New features (within scope)

### Documentation
- Build instructions
- Troubleshooting guides
- Screenshots

### Testing
- Test on real PowerPC hardware
- Test on Tiger and Leopard
- Report compatibility issues

## Development Setup

1. Xcode 2.5 (Tiger) or 3.1 (Leopard)
2. mbedTLS 2.28 compiled for PowerPC
3. Mac OS X 10.4 or 10.5 (or VM)

## Code Style

- Objective-C with Cocoa conventions
- Keep dependencies minimal
- Comment PowerPC-specific optimizations
- Test on both Tiger and Leopard

## Pull Request Checklist

- [ ] Code compiles on Xcode 2.5/3.1
- [ ] Tested on Tiger and/or Leopard
- [ ] No new external dependencies
- [ ] Follows existing code style
- [ ] Commit messages are clear

## Questions?

Open an issue for discussion before major changes.

---

**Bounty note:** Contributions may be rewarded. See [rustchain-bounties](https://github.com/Scottcjn/rustchain-bounties/issues).
