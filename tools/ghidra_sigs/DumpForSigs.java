import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import java.io.FileWriter;
import java.io.PrintWriter;

// Release-signature input (tools/make_sigs.py): instruction boundaries, bytes and outgoing references around each
// target. Args: <out file> then targets "code:<hexaddr>" (a window around it) or "data:<hexaddr>" (a window around up to
// 6 instructions that reference it). Lines: "T <kind> <target>" then "I <addr> <hexbytes> <ref,ref,...>".
public class DumpForSigs extends GhidraScript {
    void window(PrintWriter out, Address centre) throws Exception {
        Address a = centre.subtract(48);
        Instruction i = getInstructionContaining(a);
        if (i == null) i = getInstructionAfter(a);
        Address end = centre.add(160);
        while (i != null && i.getAddress().compareTo(end) < 0) {
            StringBuilder b = new StringBuilder();
            for (byte x : i.getBytes()) b.append(String.format("%02X", x));
            StringBuilder r = new StringBuilder();
            for (Reference ref : i.getReferencesFrom()) {
                if (r.length() > 0) r.append(",");
                r.append(ref.getToAddress().toString());
            }
            out.println("I " + i.getAddress() + " " + b + " " + (r.length() > 0 ? r : "-") + " ; " + i);
            i = i.getNext();
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        for (int k = 1; k < args.length; ++k) {
            String[] kv = args[k].split(":");
            Address t = currentProgram.getAddressFactory().getAddress(kv[1]);
            out.println("T " + kv[0] + " " + t);
            if (kv[0].equals("code")) {
                window(out, t);
            } else {
                int n = 0;
                for (Reference ref : getReferencesTo(t)) {
                    if (n++ >= 6) break;
                    out.println("R " + ref.getFromAddress() + " " + ref.getReferenceType());
                    window(out, ref.getFromAddress());
                }
            }
        }
        out.close();
        println("done");
    }
}
