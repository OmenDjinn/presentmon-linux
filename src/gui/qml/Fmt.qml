// SPDX-License-Identifier: MIT
// Number and text formatting shared by every page. NaN (not available) shows as an en dash.
pragma Singleton

import QtQuick

QtObject {
    readonly property string na: "–"

    // From 10 million on in short exponent form (1.2e9): only a damaged file gets there, and the
    // number must still fit a tile.
    function num(v, digits) {
        if (v === undefined || v === null || isNaN(v))
            return na
        if (Math.abs(v) >= 1e7)
            return sci(v)
        return Number(v).toLocaleString(Qt.locale(), "f", digits === undefined ? 1 : digits)
    }
    function sci(v) {
        return Number(v).toExponential(1).replace("e+", "e").replace(".", Qt.locale().decimalPoint)
    }
    // Frame rates: no decimals at 100+ fps, one below.
    function fps(v) {
        if (v === undefined || v === null || isNaN(v))
            return na
        return num(v, v >= 100 ? 0 : 1)
    }
    function ms(v) {
        if (v === undefined || v === null || isNaN(v))
            return na
        return num(v, v >= 100 ? 0 : (v >= 10 ? 1 : 2))
    }
    function pct(v) {
        if (v === undefined || v === null || isNaN(v))
            return na
        return num(v, v >= 10 ? 0 : 1)
    }
    // 75 -> "1:15", 3725 -> "1:02:05".
    function duration(seconds) {
        if (seconds === undefined || seconds === null || isNaN(seconds) || seconds < 0)
            return na
        const s = Math.floor(seconds)
        const h = Math.floor(s / 3600)
        const m = Math.floor((s % 3600) / 60)
        const r = s % 60
        const two = n => (n < 10 ? "0" : "") + n
        return h > 0 ? h + ":" + two(m) + ":" + two(r) : m + ":" + two(r)
    }
    function bytes(n) {
        if (n === undefined || n === null || isNaN(n))
            return na
        if (n < 1024)
            return n + " B"
        if (n < 1024 * 1024)
            return num(n / 1024, 0) + " KiB"
        if (n < 1024 * 1024 * 1024)
            return num(n / (1024 * 1024), 1) + " MiB"
        return num(n / (1024 * 1024 * 1024), 2) + " GiB"
    }
    function dateTime(dt) {
        if (!dt || isNaN(dt.getTime()))
            return na
        return dt.toLocaleString(Qt.locale(), Locale.ShortFormat)
    }
    // Decimals a tick step needs: 5 -> 0, 2.5 -> 1, 0.25 -> 2 (at most 6).
    function stepDecimals(step) {
        if (!(step > 0) || !isFinite(step))
            return 0
        for (let d = 0; d < 6; d++) {
            const s = step * Math.pow(10, d)
            if (Math.abs(s - Math.round(s)) < 1e-6 * Math.max(1, s))
                return d
        }
        return 6
    }
    // Axis tick, with the decimals its step needs (step 5 -> "10", 2.5 -> "7.5", 0.25 -> "1.25"). With steps
    // of 1000 and more, each value gets its own k, M or G and loses trailing zeros, as chart libraries do
    // ("500k", "1M", "1.5M"), so the labels of huge frame times still fit beside the axis.
    function tick(v, step) {
        if (v === undefined || v === null || isNaN(v))
            return na
        const a = Math.abs(v)
        if (!(step >= 1e3) || a < 1e3)
            return Number(v).toLocaleString(Qt.locale(), "f", stepDecimals(step))
        if (a >= 1e12)
            return sci(v)
        const u = a >= 1e9 ? 1e9 : (a >= 1e6 ? 1e6 : 1e3)
        const point = Qt.locale().decimalPoint
        let text = Number(v / u).toLocaleString(Qt.locale(), "f", stepDecimals(step / u))
        if (text.indexOf(point) >= 0)
            text = text.replace(/0+$/, "").replace(new RegExp("\\" + point + "$"), "")
        return text + (u === 1e9 ? "G" : (u === 1e6 ? "M" : "k"))
    }
    // Signed relative change of b against a in percent, e.g. "+3.1 %" (NaN-safe).
    function delta(a, b) {
        if (isNaN(a) || isNaN(b) || a === 0)
            return ""
        const d = (b - a) / a * 100
        return (d >= 0 ? "+" : "−") + num(Math.abs(d), Math.abs(d) >= 10 ? 0 : 1) + " %"
    }
}
