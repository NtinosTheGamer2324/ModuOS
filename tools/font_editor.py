#!/usr/bin/env python3
"""
FNT Font Editor
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog, colorchooser, font as tkfont
import json
import struct
from PIL import Image, ImageDraw, ImageFont, ImageTk
import io

class FNTFormat:
    """Custom FNT font format handler"""
    
    MAGIC = b'FNT1'
    VERSION = 1
    
    @staticmethod
    def save(font_data, filename):
        """Save font in custom FNT format"""
        with open(filename, 'wb') as f:
            # Header
            f.write(FNTFormat.MAGIC)
            f.write(struct.pack('<H', FNTFormat.VERSION))
            
            # Font metadata
            name_bytes = font_data['name'].encode('utf-8')
            f.write(struct.pack('<H', len(name_bytes)))
            f.write(name_bytes)
            
            f.write(struct.pack('<H', font_data['glyph_width']))
            f.write(struct.pack('<H', font_data['glyph_height']))
            f.write(struct.pack('<H', font_data['baseline']))
            
            # Glyph count
            glyphs = font_data['glyphs']
            f.write(struct.pack('<I', len(glyphs)))
            
            # Glyph data
            for codepoint, glyph_data in sorted(glyphs.items()):
                f.write(struct.pack('<I', codepoint))  # Unicode codepoint
                f.write(struct.pack('<H', glyph_data['width']))
                
                # Bitmap data (1 bit per pixel, packed)
                bitmap = glyph_data['bitmap']
                height = len(bitmap)
                width = len(bitmap[0]) if height > 0 else 0
                
                f.write(struct.pack('<H', width))
                f.write(struct.pack('<H', height))
                
                # Pack bits
                for row in bitmap:
                    byte_val = 0
                    bit_pos = 7
                    for pixel in row:
                        if pixel:
                            byte_val |= (1 << bit_pos)
                        bit_pos -= 1
                        if bit_pos < 0:
                            f.write(struct.pack('B', byte_val))
                            byte_val = 0
                            bit_pos = 7
                    # Write remaining bits
                    if bit_pos < 7:
                        f.write(struct.pack('B', byte_val))
    
    @staticmethod
    def load(filename):
        """Load font from custom FNT format"""
        with open(filename, 'rb') as f:
            # Verify magic
            magic = f.read(4)
            if magic != FNTFormat.MAGIC:
                raise ValueError("Invalid FNT file")
            
            version = struct.unpack('<H', f.read(2))[0]
            if version != FNTFormat.VERSION:
                raise ValueError(f"Unsupported FNT version: {version}")
            
            # Read metadata
            name_len = struct.unpack('<H', f.read(2))[0]
            name = f.read(name_len).decode('utf-8')
            
            glyph_width = struct.unpack('<H', f.read(2))[0]
            glyph_height = struct.unpack('<H', f.read(2))[0]
            baseline = struct.unpack('<H', f.read(2))[0]
            
            # Read glyphs
            glyph_count = struct.unpack('<I', f.read(4))[0]
            glyphs = {}
            
            for _ in range(glyph_count):
                codepoint = struct.unpack('<I', f.read(4))[0]
                glyph_width_actual = struct.unpack('<H', f.read(2))[0]
                width = struct.unpack('<H', f.read(2))[0]
                height = struct.unpack('<H', f.read(2))[0]
                
                # Unpack bitmap
                bitmap = []
                bytes_per_row = (width + 7) // 8
                
                for _ in range(height):
                    row = []
                    byte_data = f.read(bytes_per_row)
                    bit_pos = 7
                    byte_idx = 0
                    
                    for _ in range(width):
                        if byte_idx < len(byte_data):
                            pixel = (byte_data[byte_idx] >> bit_pos) & 1
                            row.append(pixel)
                        else:
                            row.append(0)
                        
                        bit_pos -= 1
                        if bit_pos < 0:
                            bit_pos = 7
                            byte_idx += 1
                    
                    bitmap.append(row)
                
                glyphs[codepoint] = {
                    'width': glyph_width_actual,
                    'bitmap': bitmap
                }
            
            return {
                'name': name,
                'glyph_width': glyph_width,
                'glyph_height': glyph_height,
                'baseline': baseline,
                'glyphs': glyphs
            }


class GlyphEditor(tk.Canvas):
    """'Professional' bitmap glyph editor widget with reference character overlay (I tried)"""
    
    def __init__(self, parent, width, height, pixel_size=20):
        self.grid_width = width
        self.grid_height = height
        self.pixel_size = pixel_size
        
        canvas_width = width * pixel_size + 1
        canvas_height = height * pixel_size + 1
        
        super().__init__(parent, width=canvas_width, height=canvas_height, 
                        bg='white', highlightthickness=1, highlightbackground='#cccccc')
        
        self.pixels = [[0 for _ in range(width)] for _ in range(height)]
        self.drawing = False
        self.draw_value = 1
        self.baseline = height // 2
        self.current_char = 'A'
        self.show_reference = tk.BooleanVar(value=True)
        self.reference_opacity = 0.15  # How faded the reference is
        
        self.bind('<Button-1>', self.on_mouse_down)
        self.bind('<B1-Motion>', self.on_mouse_drag)
        self.bind('<ButtonRelease-1>', self.on_mouse_up)
        
        self.draw_grid()
        self.redraw()
    
    def set_current_char(self, char):
        """Set the current character for reference display"""
        self.current_char = char
        self.redraw()
    
    def set_show_reference(self, show):
        """Toggle reference character display"""
        self.show_reference.set(show)
        self.redraw()
    
    def set_baseline(self, baseline):
        """Set the baseline position"""
        self.baseline = baseline
        self.redraw()
    
    def draw_grid(self):
        """Draw the pixel grid"""
        for i in range(self.grid_width + 1):
            x = i * self.pixel_size
            color = '#cccccc' if i % 5 != 0 else '#999999'
            self.create_line(x, 0, x, self.grid_height * self.pixel_size, fill=color)
        
        for i in range(self.grid_height + 1):
            y = i * self.pixel_size
            color = '#cccccc' if i % 5 != 0 else '#999999'
            self.create_line(0, y, self.grid_width * self.pixel_size, y, fill=color)
    
    def draw_reference_character(self):
        """Draw a faded reference character in the background"""
        if not self.show_reference.get():
            return
        
        try:
            # Create an image with the reference character
            img_width = self.grid_width * self.pixel_size
            img_height = self.grid_height * self.pixel_size
            
            # Create image
            img = Image.new('L', (img_width, img_height), 255)
            draw = ImageDraw.Draw(img)
            
            # Try to get a large font
            font_size = min(img_width, img_height) * 2 // 3
            try:
                font = ImageFont.truetype("DejaVuSans.ttf", font_size)
            except:
                try:
                    font = ImageFont.truetype("Arial.ttf", font_size)
                except:
                    try:
                        font = ImageFont.load_default()
                    except:
                        return
            
            # Get text size and center it
            bbox = draw.textbbox((0, 0), self.current_char, font=font)
            text_width = bbox[2] - bbox[0]
            text_height = bbox[3] - bbox[1]
            
            x = (img_width - text_width) // 2 - bbox[0]
            y = (img_height - text_height) // 2 - bbox[1]
            
            # Draw the character
            draw.text((x, y), self.current_char, fill=0, font=font)
            
            # Convert to PhotoImage with opacity
            # Invert colors (black text on white -> white text on black)
            img = Image.eval(img, lambda x: 255 - x)
            
            # Apply opacity
            opacity_value = int(255 * self.reference_opacity)
            img = Image.eval(img, lambda x: int(x * self.reference_opacity) if x > 0 else 0)
            
            # Convert back (now we have faded black on white)
            img = Image.eval(img, lambda x: 255 - x)
            
            # Convert to PhotoImage
            self.reference_image = ImageTk.PhotoImage(img)
            self.create_image(0, 0, anchor=tk.NW, image=self.reference_image, tags='reference')
            
            # Lower reference below grid and pixels
            self.tag_lower('reference')
            
        except Exception as e:
            # If anything fails, just skip the reference character
            pass
    
    def on_mouse_down(self, event):
        """Handle mouse down event"""
        x, y = event.x // self.pixel_size, event.y // self.pixel_size
        if 0 <= x < self.grid_width and 0 <= y < self.grid_height:
            self.drawing = True
            self.draw_value = 0 if self.pixels[y][x] else 1
            self.set_pixel(x, y, self.draw_value)
    
    def on_mouse_drag(self, event):
        """Handle mouse drag event"""
        if self.drawing:
            x, y = event.x // self.pixel_size, event.y // self.pixel_size
            if 0 <= x < self.grid_width and 0 <= y < self.grid_height:
                self.set_pixel(x, y, self.draw_value)
    
    def on_mouse_up(self, event):
        """Handle mouse up event"""
        self.drawing = False
    
    def set_pixel(self, x, y, value):
        """Set a pixel value and redraw"""
        if 0 <= x < self.grid_width and 0 <= y < self.grid_height:
            self.pixels[y][x] = value
            self.redraw_pixel(x, y)
    
    def redraw_pixel(self, x, y):
        """Redraw a single pixel"""
        x1 = x * self.pixel_size + 1
        y1 = y * self.pixel_size + 1
        x2 = x1 + self.pixel_size - 1
        y2 = y1 + self.pixel_size - 1
        
        color = 'black' if self.pixels[y][x] else 'white'
        self.create_rectangle(x1, y1, x2, y2, fill=color, outline='', tags='pixel')
    
    def redraw(self):
        """Redraw all pixels and baseline"""
        self.delete('pixel')
        self.delete('baseline')
        self.delete('reference')
        
        # Draw reference character first (in background)
        self.draw_reference_character()
        
        for y in range(self.grid_height):
            for x in range(self.grid_width):
                self.redraw_pixel(x, y)
        
        # Draw baseline
        baseline_y = self.baseline * self.pixel_size
        self.create_line(0, baseline_y, self.grid_width * self.pixel_size, 
                        baseline_y, fill='red', width=2, tags='baseline')
    
    def clear(self):
        """Clear all pixels"""
        self.pixels = [[0 for _ in range(self.grid_width)] for _ in range(self.grid_height)]
        self.redraw()
    
    def load_bitmap(self, bitmap):
        """Load bitmap data"""
        self.clear()
        for y, row in enumerate(bitmap):
            for x, pixel in enumerate(row):
                if y < self.grid_height and x < self.grid_width:
                    self.pixels[y][x] = pixel
        self.redraw()
    
    def get_bitmap(self):
        """Get current bitmap data"""
        return [row[:] for row in self.pixels]


class FontEditorApp:
    """Main font editor application"""
    
    def __init__(self, root):
        self.root = root
        self.root.title("FNT Font Editor")
        self.root.geometry("1400x800")
        
        # Font data
        self.font_name = "New Font"
        self.glyph_width = 16
        self.glyph_height = 24
        self.baseline = 18
        self.glyphs = {}
        self.current_codepoint = ord('A')
        self.current_file = None
        
        self.setup_ui()
        self.update_glyph_editor()
        
    def setup_ui(self):
        """Setup the user interface"""
        # Menu bar
        menubar = tk.Menu(self.root)
        self.root.config(menu=menubar)
        
        file_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="File", menu=file_menu)
        file_menu.add_command(label="New Font", command=self.new_font, accelerator="Ctrl+N")
        file_menu.add_command(label="Open FNT...", command=self.open_font, accelerator="Ctrl+O")
        file_menu.add_command(label="Save FNT", command=self.save_font, accelerator="Ctrl+S")
        file_menu.add_command(label="Save FNT As...", command=self.save_font_as, accelerator="Ctrl+Shift+S")
        file_menu.add_separator()
        file_menu.add_command(label="Export Preview Image...", command=self.export_preview)
        file_menu.add_separator()
        file_menu.add_command(label="Exit", command=self.root.quit)
        
        edit_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="Edit", menu=edit_menu)
        edit_menu.add_command(label="Clear Glyph", command=self.clear_glyph, accelerator="Ctrl+D")
        edit_menu.add_command(label="Copy Glyph", command=self.copy_glyph, accelerator="Ctrl+C")
        edit_menu.add_command(label="Paste Glyph", command=self.paste_glyph, accelerator="Ctrl+V")
        
        view_menu = tk.Menu(menubar, tearoff=0)
        menubar.add_cascade(label="View", menu=view_menu)
        
        # Keyboard shortcuts
        self.root.bind('<Control-n>', lambda e: self.new_font())
        self.root.bind('<Control-o>', lambda e: self.open_font())
        self.root.bind('<Control-s>', lambda e: self.save_font())
        self.root.bind('<Control-Shift-S>', lambda e: self.save_font_as())
        self.root.bind('<Control-d>', lambda e: self.clear_glyph())
        self.root.bind('<Control-c>', lambda e: self.copy_glyph())
        self.root.bind('<Control-v>', lambda e: self.paste_glyph())
        
        # Main container
        main_frame = ttk.Frame(self.root, padding=10)
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Left panel - Font properties
        left_panel = ttk.LabelFrame(main_frame, text="Font Properties", padding=10)
        left_panel.pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))
        
        ttk.Label(left_panel, text="Font Name:").pack(anchor=tk.W)
        self.font_name_entry = ttk.Entry(left_panel, width=25)
        self.font_name_entry.insert(0, self.font_name)
        self.font_name_entry.pack(fill=tk.X, pady=(0, 10))
        self.font_name_entry.bind('<KeyRelease>', lambda e: self.update_font_name())
        
        ttk.Label(left_panel, text="Glyph Dimensions:").pack(anchor=tk.W, pady=(10, 0))
        
        dim_frame = ttk.Frame(left_panel)
        dim_frame.pack(fill=tk.X, pady=5)
        
        ttk.Label(dim_frame, text="W:").pack(side=tk.LEFT)
        self.width_spinbox = ttk.Spinbox(dim_frame, from_=4, to=128, width=6)
        self.width_spinbox.set(self.glyph_width)
        self.width_spinbox.pack(side=tk.LEFT, padx=5)
        
        ttk.Label(dim_frame, text="H:").pack(side=tk.LEFT, padx=(10, 0))
        self.height_spinbox = ttk.Spinbox(dim_frame, from_=4, to=128, width=6)
        self.height_spinbox.set(self.glyph_height)
        self.height_spinbox.pack(side=tk.LEFT, padx=5)
        
        ttk.Button(left_panel, text="Apply Dimensions", 
                  command=self.apply_dimensions).pack(fill=tk.X, pady=5)
        
        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)
        
        ttk.Label(left_panel, text="Baseline:").pack(anchor=tk.W)
        self.baseline_scale = ttk.Scale(left_panel, from_=0, to=self.glyph_height-1,
                                       orient=tk.HORIZONTAL, command=self.update_baseline)
        self.baseline_scale.set(self.baseline)
        self.baseline_scale.pack(fill=tk.X, pady=5)
        
        self.baseline_label = ttk.Label(left_panel, text=f"Position: {self.baseline}")
        self.baseline_label.pack(anchor=tk.W)
        
        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)
        
        # Reference character toggle
        ttk.Label(left_panel, text="Reference Guide:").pack(anchor=tk.W, pady=(10, 5))
        self.show_reference_var = tk.BooleanVar(value=True)
        self.show_reference_check = ttk.Checkbutton(left_panel, text="Show reference character",
                                                    variable=self.show_reference_var,
                                                    command=self.toggle_reference)
        self.show_reference_check.pack(anchor=tk.W)
        
        ttk.Separator(left_panel, orient=tk.HORIZONTAL).pack(fill=tk.X, pady=10)
        
        ttk.Label(left_panel, text="Font Statistics:").pack(anchor=tk.W, pady=(10, 5))
        self.stats_label = ttk.Label(left_panel, text="Glyphs: 0", font=('', 9))
        self.stats_label.pack(anchor=tk.W)
        
        # Center panel - Glyph editor
        center_panel = ttk.LabelFrame(main_frame, text="Glyph Editor", padding=10)
        center_panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 10))
        
        # Character selector
        char_frame = ttk.Frame(center_panel)
        char_frame.pack(fill=tk.X, pady=(0, 10))
        
        ttk.Label(char_frame, text="Character:").pack(side=tk.LEFT)
        
        self.char_entry = ttk.Entry(char_frame, width=5)
        self.char_entry.insert(0, chr(self.current_codepoint))
        self.char_entry.pack(side=tk.LEFT, padx=5)
        self.char_entry.bind('<Return>', lambda e: self.change_character())
        
        ttk.Button(char_frame, text="Go", command=self.change_character).pack(side=tk.LEFT)
        
        ttk.Label(char_frame, text="Unicode:").pack(side=tk.LEFT, padx=(20, 5))
        self.unicode_entry = ttk.Entry(char_frame, width=8)
        self.unicode_entry.insert(0, f"U+{self.current_codepoint:04X}")
        self.unicode_entry.pack(side=tk.LEFT, padx=5)
        self.unicode_entry.bind('<Return>', lambda e: self.change_unicode())
        
        ttk.Button(char_frame, text="Go", command=self.change_unicode).pack(side=tk.LEFT)
        
        # Navigation buttons
        nav_frame = ttk.Frame(center_panel)
        nav_frame.pack(fill=tk.X, pady=(0, 10))
        
        ttk.Button(nav_frame, text="◄◄ Prev", command=self.prev_char).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Next ►►", command=self.next_char).pack(side=tk.LEFT, padx=2)
        
        ttk.Separator(nav_frame, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=10)
        
        ttk.Button(nav_frame, text="Clear", command=self.clear_glyph).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Copy", command=self.copy_glyph).pack(side=tk.LEFT, padx=2)
        ttk.Button(nav_frame, text="Paste", command=self.paste_glyph).pack(side=tk.LEFT, padx=2)
        
        # Glyph editor canvas
        editor_container = ttk.Frame(center_panel)
        editor_container.pack(fill=tk.BOTH, expand=True)
        
        self.glyph_editor = GlyphEditor(editor_container, self.glyph_width, 
                                       self.glyph_height, pixel_size=20)
        self.glyph_editor.pack()
        
        # Right panel - Character sets and preview
        right_panel = ttk.Frame(main_frame)
        right_panel.pack(side=tk.LEFT, fill=tk.BOTH)
        
        # Quick character sets with scrolling
        sets_frame = ttk.LabelFrame(right_panel, text="Quick Sets", padding=10)
        sets_frame.pack(fill=tk.BOTH, expand=True, pady=(0, 10))
        
        # Create scrollable frame for character sets
        sets_canvas = tk.Canvas(sets_frame, height=400, width=180)
        sets_scrollbar = ttk.Scrollbar(sets_frame, orient="vertical", command=sets_canvas.yview)
        sets_scrollable = ttk.Frame(sets_canvas)
        
        sets_scrollable.bind(
            "<Configure>",
            lambda e: sets_canvas.configure(scrollregion=sets_canvas.bbox("all"))
        )
        
        sets_canvas.create_window((0, 0), window=sets_scrollable, anchor="nw")
        sets_canvas.configure(yscrollcommand=sets_scrollbar.set)
        
        sets_canvas.pack(side="left", fill="both", expand=True)
        sets_scrollbar.pack(side="right", fill="y")
        
        # Basic Latin
        ttk.Label(sets_scrollable, text="Basic Latin:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(5, 2))
        ttk.Button(sets_scrollable, text="A-Z", width=15,
                  command=lambda: self.load_character_set(range(ord('A'), ord('Z')+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="a-z", width=15,
                  command=lambda: self.load_character_set(range(ord('a'), ord('z')+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="0-9", width=15,
                  command=lambda: self.load_character_set(range(ord('0'), ord('9')+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Punctuation", width=15,
                  command=lambda: self.load_character_set(list(range(33, 48)) + list(range(58, 65)) + 
                                                         list(range(91, 97)) + list(range(123, 127)))).pack(pady=1)
        ttk.Button(sets_scrollable, text="ASCII (32-126)", width=15,
                  command=lambda: self.load_character_set(range(32, 127))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Extended ASCII", width=15,
                  command=lambda: self.load_character_set(range(128, 256))).pack(pady=1)
        
        # Greek
        ttk.Label(sets_scrollable, text="Greek:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Greek Uppercase", width=15,
                  command=lambda: self.load_character_set(range(0x0391, 0x03A9+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Greek Lowercase", width=15,
                  command=lambda: self.load_character_set(range(0x03B1, 0x03C9+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Greek Full", width=15,
                  command=lambda: self.load_character_set(range(0x0370, 0x03FF+1))).pack(pady=1)
        
        # Cyrillic
        ttk.Label(sets_scrollable, text="Cyrillic:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Cyrillic Uppercase", width=15,
                  command=lambda: self.load_character_set(range(0x0410, 0x042F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Cyrillic Lowercase", width=15,
                  command=lambda: self.load_character_set(range(0x0430, 0x044F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Cyrillic Full", width=15,
                  command=lambda: self.load_character_set(range(0x0400, 0x04FF+1))).pack(pady=1)
        
        # Extended Latin
        ttk.Label(sets_scrollable, text="Extended Latin:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Latin-1 Suppl.", width=15,
                  command=lambda: self.load_character_set(range(0x00A0, 0x00FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Latin Extended-A", width=15,
                  command=lambda: self.load_character_set(range(0x0100, 0x017F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Latin Extended-B", width=15,
                  command=lambda: self.load_character_set(range(0x0180, 0x024F+1))).pack(pady=1)
        
        # Symbols and Special
        ttk.Label(sets_scrollable, text="Symbols:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Math Operators", width=15,
                  command=lambda: self.load_character_set(range(0x2200, 0x22FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Arrows", width=15,
                  command=lambda: self.load_character_set(range(0x2190, 0x21FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Box Drawing", width=15,
                  command=lambda: self.load_character_set(range(0x2500, 0x257F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Block Elements", width=15,
                  command=lambda: self.load_character_set(range(0x2580, 0x259F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Geometric Shapes", width=15,
                  command=lambda: self.load_character_set(range(0x25A0, 0x25FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Misc Symbols", width=15,
                  command=lambda: self.load_character_set(range(0x2600, 0x26FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Dingbats", width=15,
                  command=lambda: self.load_character_set(range(0x2700, 0x27BF+1))).pack(pady=1)
        
        # Currency
        ttk.Label(sets_scrollable, text="Currency:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Currency Symbols", width=15,
                  command=lambda: self.load_character_set(range(0x20A0, 0x20CF+1))).pack(pady=1)
        
        # Asian Scripts
        ttk.Label(sets_scrollable, text="Asian:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Hiragana", width=15,
                  command=lambda: self.load_character_set(range(0x3040, 0x309F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Katakana", width=15,
                  command=lambda: self.load_character_set(range(0x30A0, 0x30FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Hangul Jamo", width=15,
                  command=lambda: self.load_character_set(range(0x1100, 0x11FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="CJK Unified", width=15,
                  command=lambda: self.load_character_set(range(0x4E00, 0x4F00))).pack(pady=1)
        
        # Other Scripts
        ttk.Label(sets_scrollable, text="Other Scripts:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Hebrew", width=15,
                  command=lambda: self.load_character_set(range(0x0590, 0x05FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Arabic", width=15,
                  command=lambda: self.load_character_set(range(0x0600, 0x06FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Devanagari", width=15,
                  command=lambda: self.load_character_set(range(0x0900, 0x097F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Thai", width=15,
                  command=lambda: self.load_character_set(range(0x0E00, 0x0E7F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Tibetan", width=15,
                  command=lambda: self.load_character_set(range(0x0F00, 0x0FFF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Armenian", width=15,
                  command=lambda: self.load_character_set(range(0x0530, 0x058F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Georgian", width=15,
                  command=lambda: self.load_character_set(range(0x10A0, 0x10FF+1))).pack(pady=1)
        
        # Emojis (Basic)
        ttk.Label(sets_scrollable, text="Emojis:", font=('', 9, 'bold')).pack(anchor=tk.W, pady=(10, 2))
        ttk.Button(sets_scrollable, text="Emoticons", width=15,
                  command=lambda: self.load_character_set(range(0x1F600, 0x1F64F+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Misc Symbols", width=15,
                  command=lambda: self.load_character_set(range(0x1F300, 0x1F5FF+1))).pack(pady=1)
        ttk.Button(sets_scrollable, text="Transport", width=15,
                  command=lambda: self.load_character_set(range(0x1F680, 0x1F6FF+1))).pack(pady=1)
        
        # Preview
        preview_frame = ttk.LabelFrame(right_panel, text="Preview", padding=10)
        preview_frame.pack(fill=tk.BOTH, expand=True)
        
        ttk.Label(preview_frame, text="Sample Text:").pack(anchor=tk.W)
        self.preview_text = ttk.Entry(preview_frame)
        self.preview_text.insert(0, "The quick brown fox")
        self.preview_text.pack(fill=tk.X, pady=(0, 10))
        self.preview_text.bind('<KeyRelease>', lambda e: self.update_preview())
        
        preview_canvas_frame = ttk.Frame(preview_frame, relief=tk.SUNKEN, borderwidth=1)
        preview_canvas_frame.pack(fill=tk.BOTH, expand=True)
        
        self.preview_canvas = tk.Canvas(preview_canvas_frame, bg='white', height=100)
        self.preview_canvas.pack(fill=tk.BOTH, expand=True)
        
        ttk.Button(preview_frame, text="Update Preview", 
                  command=self.update_preview).pack(fill=tk.X, pady=(10, 0))
        
        # Status bar
        self.status_var = tk.StringVar(value="Ready")
        status_bar = ttk.Label(self.root, textvariable=self.status_var, 
                              relief=tk.SUNKEN, anchor=tk.W)
        status_bar.pack(side=tk.BOTTOM, fill=tk.X)
        
        self.clipboard_bitmap = None
    
    def toggle_reference(self):
        """Toggle reference character display"""
        self.glyph_editor.set_show_reference(self.show_reference_var.get())
        self.glyph_editor.redraw()
    
    def update_font_name(self):
        """Update font name from entry"""
        self.font_name = self.font_name_entry.get()
    
    def apply_dimensions(self):
        """Apply new glyph dimensions"""
        try:
            new_width = int(self.width_spinbox.get())
            new_height = int(self.height_spinbox.get())
            
            if new_width < 4 or new_height < 4:
                messagebox.showerror("Error", "Dimensions must be at least 4x4")
                return
            
            if messagebox.askyesno("Confirm", 
                                  "Changing dimensions will clear all glyphs. Continue?"):
                self.glyph_width = new_width
                self.glyph_height = new_height
                self.baseline = new_height // 2
                self.glyphs = {}
                
                # Recreate editor
                self.glyph_editor.destroy()
                editor_container = self.glyph_editor.master
                self.glyph_editor = GlyphEditor(editor_container, self.glyph_width, 
                                               self.glyph_height, pixel_size=20)
                self.glyph_editor.pack()
                
                self.baseline_scale.config(to=self.glyph_height-1)
                self.baseline_scale.set(self.baseline)
                self.update_baseline(self.baseline)
                self.update_stats()
                self.status_var.set("Dimensions updated")
        except ValueError:
            messagebox.showerror("Error", "Invalid dimensions")
    
    def update_baseline(self, value):
        """Update baseline position"""
        self.baseline = int(float(value))
        self.baseline_label.config(text=f"Position: {self.baseline}")
        self.glyph_editor.set_baseline(self.baseline)
    
    def save_current_glyph(self):
        """Save current glyph to font data"""
        bitmap = self.glyph_editor.get_bitmap()
        # Calculate actual width (trim right whitespace)
        actual_width = 0
        for row in bitmap:
            for x in range(len(row)-1, -1, -1):
                if row[x]:
                    actual_width = max(actual_width, x + 1)
                    break
        
        if actual_width == 0:
            actual_width = self.glyph_width // 4  # Minimum width for empty glyphs
        
        self.glyphs[self.current_codepoint] = {
            'width': actual_width,
            'bitmap': bitmap
        }
        self.update_stats()
    
    def update_glyph_editor(self):
        """Update glyph editor with current character"""
        if self.current_codepoint in self.glyphs:
            self.glyph_editor.load_bitmap(self.glyphs[self.current_codepoint]['bitmap'])
        else:
            self.glyph_editor.clear()
        
        # Update reference character - handle special cases
        try:
            char = chr(self.current_codepoint)
            # For control characters and other non-printable, don't show reference
            if self.current_codepoint < 32 or self.current_codepoint == 127:
                self.glyph_editor.set_current_char('□')
            elif 128 <= self.current_codepoint <= 159:
                # C1 control characters (extended ASCII control range)
                self.glyph_editor.set_current_char('□')
            else:
                self.glyph_editor.set_current_char(char)
        except:
            self.glyph_editor.set_current_char('?')
        
        self.char_entry.delete(0, tk.END)
        try:
            char = chr(self.current_codepoint)
            # Show printable characters in the entry
            if self.current_codepoint >= 32 and self.current_codepoint != 127:
                if not (128 <= self.current_codepoint <= 159):  # Skip C1 controls
                    self.char_entry.insert(0, char)
                else:
                    self.char_entry.insert(0, f"[{self.current_codepoint}]")
            else:
                self.char_entry.insert(0, f"[{self.current_codepoint}]")
        except:
            self.char_entry.insert(0, "?")
        
        self.unicode_entry.delete(0, tk.END)
        self.unicode_entry.insert(0, f"U+{self.current_codepoint:04X}")
        
        # Get character description
        char_desc = self.get_char_description(self.current_codepoint)
        glyph_exists = "Exists" if self.current_codepoint in self.glyphs else "New"
        self.status_var.set(f"Editing U+{self.current_codepoint:04X} ({char_desc}) - {glyph_exists}")
    
    def get_char_description(self, codepoint):
        """Get a description for a character codepoint"""
        if codepoint < 32:
            control_names = {
                0: "NULL", 9: "TAB", 10: "LF", 13: "CR", 27: "ESC"
            }
            return control_names.get(codepoint, f"Control-{codepoint}")
        elif codepoint == 127:
            return "DEL"
        elif 128 <= codepoint <= 159:
            return f"C1-Control-{codepoint}"
        else:
            try:
                return chr(codepoint)
            except:
                return "?"
    
    def change_character(self):
        """Change current character from char entry"""
        self.save_current_glyph()
        char = self.char_entry.get()
        if char:
            self.current_codepoint = ord(char[0])
            self.update_glyph_editor()
    
    def change_unicode(self):
        """Change current character from unicode entry"""
        self.save_current_glyph()
        unicode_str = self.unicode_entry.get().strip()
        try:
            if unicode_str.startswith('U+') or unicode_str.startswith('u+'):
                unicode_str = unicode_str[2:]
            self.current_codepoint = int(unicode_str, 16)
            self.update_glyph_editor()
        except ValueError:
            messagebox.showerror("Error", "Invalid Unicode value")
    
    def prev_char(self):
        """Go to previous character"""
        self.save_current_glyph()
        self.current_codepoint = max(0, self.current_codepoint - 1)
        self.update_glyph_editor()
    
    def next_char(self):
        """Go to next character"""
        self.save_current_glyph()
        self.current_codepoint = min(0x10FFFF, self.current_codepoint + 1)
        self.update_glyph_editor()
    
    def clear_glyph(self):
        """Clear current glyph"""
        self.glyph_editor.clear()
        if self.current_codepoint in self.glyphs:
            del self.glyphs[self.current_codepoint]
            self.update_stats()
        self.status_var.set("Glyph cleared")
    
    def copy_glyph(self):
        """Copy current glyph to clipboard"""
        self.clipboard_bitmap = self.glyph_editor.get_bitmap()
        self.status_var.set("Glyph copied")
    
    def paste_glyph(self):
        """Paste glyph from clipboard"""
        if self.clipboard_bitmap:
            self.glyph_editor.load_bitmap(self.clipboard_bitmap)
            self.status_var.set("Glyph pasted")
        else:
            messagebox.showinfo("Info", "No glyph in clipboard")
    
    def load_character_set(self, char_range):
        """Start editing a character set"""
        self.save_current_glyph()
        char_list = list(char_range)
        self.current_codepoint = char_list[0]
        self.update_glyph_editor()
        messagebox.showinfo("Character Set", 
                          f"Loaded character set. Use Next/Prev to navigate.\n"
                          f"Range: U+{min(char_list):04X} to U+{max(char_list):04X}\n"
                          f"Total characters: {len(char_list)}")
    
    def update_preview(self):
        """Update font preview"""
        self.save_current_glyph()
        
        text = self.preview_text.get()
        if not text or not self.glyphs:
            self.preview_canvas.delete('all')
            return
        
        # Create preview image
        margin = 5
        x = margin
        y = margin
        
        self.preview_canvas.delete('all')
        
        for char in text:
            codepoint = ord(char)
            if codepoint in self.glyphs:
                glyph = self.glyphs[codepoint]
                bitmap = glyph['bitmap']
                
                # Draw glyph
                for gy, row in enumerate(bitmap):
                    for gx, pixel in enumerate(row):
                        if pixel:
                            self.preview_canvas.create_rectangle(
                                x + gx * 2, y + gy * 2,
                                x + gx * 2 + 2, y + gy * 2 + 2,
                                fill='black', outline=''
                            )
                
                x += glyph['width'] * 2 + 2
            else:
                # Unknown character - draw box
                self.preview_canvas.create_rectangle(
                    x, y, x + self.glyph_width * 2, y + self.glyph_height * 2,
                    outline='red'
                )
                x += self.glyph_width * 2 + 2
    
    def update_stats(self):
        """Update font statistics display"""
        self.stats_label.config(text=f"Glyphs: {len(self.glyphs)}")
    
    def new_font(self):
        """Create a new font"""
        if messagebox.askyesno("New Font", "Create new font? Unsaved changes will be lost."):
            self.font_name = "New Font"
            self.glyphs = {}
            self.current_file = None
            self.font_name_entry.delete(0, tk.END)
            self.font_name_entry.insert(0, self.font_name)
            self.update_glyph_editor()
            self.update_stats()
            self.status_var.set("New font created")
    
    def open_font(self):
        """Open an existing FNT file"""
        filename = filedialog.askopenfilename(
            title="Open Font",
            filetypes=[("FNT Files", "*.fnt"), ("All Files", "*.*")]
        )
        
        if filename:
            try:
                font_data = FNTFormat.load(filename)
                
                self.font_name = font_data['name']
                self.glyph_width = font_data['glyph_width']
                self.glyph_height = font_data['glyph_height']
                self.baseline = font_data['baseline']
                self.glyphs = font_data['glyphs']
                self.current_file = filename
                
                # Update UI
                self.font_name_entry.delete(0, tk.END)
                self.font_name_entry.insert(0, self.font_name)
                
                self.width_spinbox.set(self.glyph_width)
                self.height_spinbox.set(self.glyph_height)
                self.baseline_scale.config(to=self.glyph_height-1)
                self.baseline_scale.set(self.baseline)
                
                # Recreate editor
                self.glyph_editor.destroy()
                editor_container = self.glyph_editor.master
                self.glyph_editor = GlyphEditor(editor_container, self.glyph_width, 
                                               self.glyph_height, pixel_size=20)
                self.glyph_editor.pack()
                self.glyph_editor.set_baseline(self.baseline)
                
                self.update_glyph_editor()
                self.update_stats()
                self.status_var.set(f"Loaded: {filename}")
                
            except Exception as e:
                messagebox.showerror("Error", f"Failed to load font:\n{str(e)}")
    
    def save_font(self):
        """Save font to current file"""
        if self.current_file:
            self.save_font_to(self.current_file)
        else:
            self.save_font_as()
    
    def save_font_as(self):
        """Save font to new file"""
        filename = filedialog.asksaveasfilename(
            title="Save Font As",
            defaultextension=".fnt",
            filetypes=[("FNT Files", "*.fnt"), ("All Files", "*.*")]
        )
        
        if filename:
            self.current_file = filename
            self.save_font_to(filename)
    
    def save_font_to(self, filename):
        """Save font to specified file"""
        self.save_current_glyph()
        self.update_font_name()
        
        try:
            font_data = {
                'name': self.font_name,
                'glyph_width': self.glyph_width,
                'glyph_height': self.glyph_height,
                'baseline': self.baseline,
                'glyphs': self.glyphs
            }
            
            FNTFormat.save(font_data, filename)
            self.status_var.set(f"Saved: {filename}")
            messagebox.showinfo("Success", f"Font saved successfully!\n{len(self.glyphs)} glyphs")
            
        except Exception as e:
            messagebox.showerror("Error", f"Failed to save font:\n{str(e)}")
    
    def export_preview(self):
        """Export preview image of the font"""
        filename = filedialog.asksaveasfilename(
            title="Export Preview",
            defaultextension=".png",
            filetypes=[("PNG Image", "*.png"), ("All Files", "*.*")]
        )
        
        if filename:
            self.save_current_glyph()
            
            try:
                # Create preview of all glyphs
                chars_per_row = 16
                rows = (len(self.glyphs) + chars_per_row - 1) // chars_per_row
                
                img_width = chars_per_row * (self.glyph_width + 4)
                img_height = rows * (self.glyph_height + 4)
                
                img = Image.new('RGB', (img_width, img_height), 'white')
                draw = ImageDraw.Draw(img)
                
                sorted_glyphs = sorted(self.glyphs.items())
                
                for idx, (codepoint, glyph) in enumerate(sorted_glyphs):
                    row = idx // chars_per_row
                    col = idx % chars_per_row
                    
                    x_offset = col * (self.glyph_width + 4) + 2
                    y_offset = row * (self.glyph_height + 4) + 2
                    
                    bitmap = glyph['bitmap']
                    for y, brow in enumerate(bitmap):
                        for x, pixel in enumerate(brow):
                            if pixel:
                                draw.point((x_offset + x, y_offset + y), fill='black')
                
                img.save(filename)
                self.status_var.set(f"Preview exported: {filename}")
                messagebox.showinfo("Success", "Preview image exported successfully!")
                
            except Exception as e:
                messagebox.showerror("Error", f"Failed to export preview:\n{str(e)}")


def main():
    """Main application entry point"""
    root = tk.Tk()
    app = FontEditorApp(root)
    root.mainloop()


if __name__ == '__main__':
    main()