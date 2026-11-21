// ImportSymbols.java
// Apply profiles/spongebob/analysis/symbols/symbols.csv (tools/symbolize) to the
// open program: creates/renames functions and puts "Class::Method" names in a
// Ghidra class namespace. Assumes the program is loaded at the runtime base
// (0x08804000), the same addresses the CSV uses.
// @category PSPRecomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.app.util.NamespaceUtils;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

public class ImportSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        File input = askFile("symbols.csv from tools/symbolize", "Import");
        int renamed = 0, created = 0, failed = 0;
        try (BufferedReader reader = new BufferedReader(new FileReader(input))) {
            String line = reader.readLine();  // header
            while ((line = reader.readLine()) != null && !monitor.isCancelled()) {
                String[] cols = line.split(",", 3);
                if (cols.length < 2) continue;
                Address address = toAddr(Long.decode(cols[0]));
                String full = cols[1];
                String name = full;
                Namespace ns = currentProgram.getGlobalNamespace();
                int split = full.lastIndexOf("::");
                if (split > 0) {
                    name = full.substring(split + 2);
                    ns = NamespaceUtils.createNamespaceHierarchy(full.substring(0, split), null,
                                                                 currentProgram, SourceType.ANALYSIS);
                }
                try {
                    Function function = getFunctionAt(address);
                    if (function == null) {
                        function = createFunction(address, null);
                        if (function == null) { failed++; continue; }
                        created++;
                    }
                    function.setName(name.replace(' ', '_'), SourceType.ANALYSIS);
                    function.setParentNamespace(ns);
                    renamed++;
                } catch (Exception e) {
                    failed++;
                }
            }
        }
        println("ImportSymbols: renamed " + renamed + " (created " + created + "), failed " + failed);
    }
}
